#include "ColonySim/Skills.h"

#include <algorithm>

namespace colony {

SkillRules SkillRules::read(const DefDatabase& defs) {
    SkillRules r;
    const Def* def = defs.find("Rules", "SkillRules");
    if (def == nullptr) {
        return r;
    }
    r.present = true;
    for (size_t i = 0; i < def->count("xpToNextLevel"); ++i) {
        r.xpToNextLevel.push_back(def->numberAt("xpToNextLevel", i, 0.0));
    }
    r.passionLearn[0] = def->number("passionLearnFactor.None", r.passionLearn[0]);
    r.passionLearn[1] = def->number("passionLearnFactor.Minor", r.passionLearn[1]);
    r.passionLearn[2] = def->number("passionLearnFactor.Major", r.passionLearn[2]);
    r.passionMood[1] = def->number("passionMood.Minor", r.passionMood[1]);
    r.passionMood[2] = def->number("passionMood.Major", r.passionMood[2]);
    r.dailyXpSoftCap = def->number("dailyXpSoftCap", r.dailyXpSoftCap);
    r.xpFactorOverCap = def->number("xpFactorOverCap", r.xpFactorOverCap);
    for (size_t i = 0; i < def->count("decayXpPerDay"); ++i) {
        r.decayXpPerDay.push_back(def->numberAt("decayXpPerDay", i, 0.0));
    }
    r.xpBelowZeroBeforeLevelLoss = def->number("xpBelowZeroBeforeLevelLoss", r.xpBelowZeroBeforeLevelLoss);
    return r;
}

double learnSkillXp(const SkillRules& rules, SkillRecord& skill, double xp) {
    const int tableTop = rules.maxLevel();
    if (skill.disabled || tableTop < 1 || !(xp > 0.0)) {
        return 0.0;
    }
    // A skill trait's bonus lets the skill pass the usual top by that much; levels past the table
    // cost what the top level costs.
    const int top = tableTop + std::max(0, skill.bonus);
    const auto cost = [&](int level) { return static_cast<float>(rules.xpToNextLevel[static_cast<size_t>(std::min(level, tableTop))]); };
    double factor = rules.passionLearn[std::min(2, static_cast<int>(skill.passion))];
    if (static_cast<double>(skill.xpToday) >= rules.dailyXpSoftCap) {
        factor *= rules.xpFactorOverCap;
    }
    const double added = xp * factor;
    skill.xp += static_cast<float>(added);
    skill.xpToday += static_cast<float>(added);
    while (skill.level < top && skill.xp >= cost(skill.level)) {
        skill.xp -= cost(skill.level);
        ++skill.level;
    }
    if (skill.level >= top) {
        skill.xp = std::min(skill.xp, cost(top) - 1.0f);
    }
    return added;
}

void skillDayPassed(const SkillRules& rules, SkillRecord& skill, float lossFactor) {
    skill.xpToday = 0.0f;
    const int top = rules.maxLevel();
    if (skill.disabled || top < 1 || skill.level < 0 || rules.decayXpPerDay.empty() ||
        (static_cast<size_t>(skill.level) >= rules.decayXpPerDay.size() && skill.level <= top)) {
        return;
    }
    // Levels past the table (a skill trait's bonus) lose what the top level loses.
    const size_t row = std::min(static_cast<size_t>(skill.level), rules.decayXpPerDay.size() - 1);
    const float decay = static_cast<float>(rules.decayXpPerDay[row]) * lossFactor;
    skill.xp -= decay;
    skill.xpToday -= decay;  // the cap counts net experience: the day starts with the loss already in
    while (skill.level > 0 && skill.xp < -static_cast<float>(rules.xpBelowZeroBeforeLevelLoss)) {
        --skill.level;
        skill.xp += static_cast<float>(rules.xpToNextLevel[std::min(static_cast<size_t>(skill.level), rules.xpToNextLevel.size() - 1)]);
    }
    if (skill.level == 0) {
        skill.xp = std::max(skill.xp, 0.0f);
    }
}

}  // namespace colony

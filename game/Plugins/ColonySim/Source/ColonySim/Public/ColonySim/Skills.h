#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"

#include <vector>

namespace colony {

// Skill learning (task C12). See docs/design/skills.md.
//
// The numbers of the SkillRules def (data/defs/Rules) and the two things that change a skill's
// experience: `learnSkillXp` (work done) and `skillDayPassed` (the daily reset and the slow loss of
// high levels). Plain functions on a SkillRecord, so the job system, the tests and later tasks
// (cooking, doctoring, hunting...) all learn the same way.

struct COLONY_API SkillRules {
    bool present = false;
    std::vector<double> xpToNextLevel;       // by level; the last entry is the top level's
    double passionLearn[3] = {0.35, 1.0, 1.5};  // by Passion: None, Minor, Major
    double passionMood[3] = {0.0, 8.0, 14.0};   // mood while working at a skill with that passion
    double dailyXpSoftCap = 4000.0;          // experience in one skill per day before it slows
    double xpFactorOverCap = 0.2;            // the slowed rate
    std::vector<double> decayXpPerDay;       // by level; no entry means none
    double xpBelowZeroBeforeLevelLoss = 1000.0;

    int maxLevel() const { return static_cast<int>(xpToNextLevel.size()) - 1; }
    static SkillRules read(const DefDatabase& defs);
};

// Adds `xp` (the work's experience before passion) to a skill: times the passion's factor, times
// the over-cap factor once the skill has earned the daily soft cap today, then levels up. A
// disabled skill learns nothing. At the top level experience piles up to just under a level's
// worth, which is what holds off the decay. Returns the experience that was added.
COLONY_API double learnSkillXp(const SkillRules& rules, SkillRecord& skill, double xp);

// Midnight for one skill: today's total starts again, and from level 10 up the skill loses
// `decayXpPerDay[level]`. Experience may sink `xpBelowZeroBeforeLevelLoss` below zero before the
// level is lost; the level below then takes over what is left.
COLONY_API void skillDayPassed(const SkillRules& rules, SkillRecord& skill);

}  // namespace colony

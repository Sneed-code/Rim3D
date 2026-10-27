#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace colony {

class HealthSystem;
class StateHasher;
class Things;
struct GameTime;

// Inspirations (task C25). See docs/design/inspirations.md.
//
// A colonist in a good mood is sometimes inspired: for a couple of days one thing it does goes far better
// (a craft of legendary quality, a recruitment that works, a frenzy of shooting). The Inspirations defs say what
// each one does; this system rolls them for colonists, keeps the one a pawn has, ends it when its time is up or it
// has been used, and answers the systems that apply the effects. Unreal does not include this header: it reads
// the events InspirationStarted and InspirationEnded.

// On a pawn that is inspired. Every field is saved.
struct Inspired {
    std::string def;       // the Inspirations def, by defName
    int64_t startedAt = 0;  // game second
    int64_t endsAt = 0;     // game second it fades if nothing used it
};

// The numbers from the InspirationRules def (data/defs/Rules). `present` is false without it, and then pawns are
// never inspired by themselves (they can still be given one).
struct COLONY_API InspirationRules {
    bool present = false;
    int checkEvery = 2880;       // game seconds between a pawn's chances; RimWorld's 2000 ticks
    float minMood = 50.0f;       // of the 0 to 100 scale; under it a pawn is never inspired
    // (mood, mean days between inspirations), x ascending; the mean is read by linear interpolation.
    std::vector<std::pair<float, float>> meanDays = {{50.0f, 210.0f}, {100.0f, 15.0f}};
    double defaultDays = 2.5;    // for an Inspirations def without `durationDays`

    static InspirationRules read(const DefDatabase& defs);
};

class COLONY_API InspirationSystem {
public:
    InspirationSystem(Things& things, HealthSystem& health, const DefDatabase& defs, const GameTime& time);
    InspirationSystem(const InspirationSystem&) = delete;
    InspirationSystem& operator=(const InspirationSystem&) = delete;

    const InspirationRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    // Whether colonists become inspired by themselves (default yes). Off for tests of something else that run for days.
    void setNatural(bool natural) { natural_ = natural; }

    // The pawn's inspiration (an Inspirations defName), or empty.
    std::string defOf(Entity pawn) const;
    bool has(Entity pawn) const { return !defOf(pawn).empty(); }
    // Whether this Inspirations def could inspire the pawn: its skills are not all disabled for the pawn.
    bool eligible(Entity pawn, const std::string& def) const;

    // Gives a pawn an inspiration now (dev entry point and tests). False, with a reason in plain words, if the def is
    // unknown, the pawn is no colonist, is already inspired, or can't use it.
    bool start(Entity pawn, const std::string& def, std::string& error);
    // Takes it away. `why` goes in the event ("used", "expired", "dismissed").
    bool end(Entity pawn, const char* why);

    // ---- What an inspiration does ------------------------------------------------------------------
    // The factor an active inspiration puts on a stat ("aimSpread", "fireCooldown", "tradePrice"); 1 for none.
    float statFactor(Entity pawn, const char* stat) const;
    // The quality levels the pawn's next crafted thing gets on top of its roll (Inspired Creativity: 2). Using it
    // ends the inspiration, so ask only when something with a quality is being made.
    int takeQualityShift(Entity pawn);
    // Whether the pawn's next tend keeps its medicine (Inspired Surgery); using it ends the inspiration.
    bool takeFreeMedicine(Entity pawn);
    // The factor on a recruit chance the warden's next attempt gets (Inspired Recruitment); using it ends the
    // inspiration. 1 when it has none.
    float takeRecruitFactor(Entity pawn);

    // One game second: chances for colonists, and inspirations whose time is up end.
    void step(int64_t now);

    // After loading: checks what the save says. False, with `error`, when it can't be.
    bool afterLoad(std::string& error);
    void hashThing(Entity e, StateHasher& h) const;

private:
    struct Kind {
        DefId def;
        std::string name;
        double commonality = 1.0;
        double days = 2.5;
        std::vector<std::string> anySkill;  // at least one must be enabled for the pawn; none: any pawn
        std::vector<std::pair<std::string, float>> statFactors;
        int qualityShift = 0;
        bool freeMedicine = false;
        float recruitFactor = 1.0f;
    };

    const Kind* kindNamed(const std::string& name) const;
    const Kind* activeKind(Entity pawn) const;
    bool colonist(Entity pawn) const;
    void natural(Entity pawn, int64_t now);
    void begin(Entity pawn, const Kind& kind, int64_t now);
    void emit(EventType type, Entity pawn, const std::string& message, float amount);

    Things& things_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    InspirationRules rules_;
    std::vector<Kind> kinds_;
    std::vector<Event>* events_ = nullptr;
    bool natural_ = true;
};

}  // namespace colony

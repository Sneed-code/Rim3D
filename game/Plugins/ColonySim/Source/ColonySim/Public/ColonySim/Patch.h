#pragma once

// Patching (body health part B, docs/design/body-health.md): a person stops and patches a bleeding body zone itself,
// 1, 2 or 3 real seconds for a light, moderate or heavy bleed, which tends that zone's bleeding wounds. The AI patches
// only when it is safe (no enemy seen for a moment); a fighter bleeding under 1500 blood falls back to cover first
// (FighterSystem::badlyHurt). The player's pawn patches on PatchCommand.

#include "ColonySim/Api.h"
#include "ColonySim/Entity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

class FighterSystem;
class HealthSystem;
class Things;
struct Event;
struct GameTime;
class StateHasher;

// On a pawn while it patches: the zone (HealthSystem::zoneNames order) and the steps left. Saved (section "patch").
struct Patching {
    int zone = -1;
    int stepsLeft = 0;
    bool player = false;  // started by PatchCommand
};

struct PatchRules {
    int safeSeenSteps = 60;   // a fighter patches only when no enemy within safeRange was seen for this long (2 real s)
    float safeRange = 30.0f;  // metres
    int thinkEvery = 15;     // steps between the AI's looks (offset by the pawn's id)
};

class COLONY_API PatchSystem {
public:
    PatchSystem(Things& things, HealthSystem& health, FighterSystem& fighters, const GameTime& time);

    void setEventLog(std::vector<Event>* events) { events_ = events; }
    const PatchRules& rules() const { return rules_; }

    // Starts patching the pawn's worst bleeding zone. False, with a reason, if it has no bleeding zone, is downed,
    // or is patching already.
    bool patch(Entity pawn, bool player, std::string& error);
    bool patching(Entity pawn) const;
    // Whether the AI would think it safe for this pawn to patch now.
    bool safe(Entity pawn) const;

    void step();
    void hashThing(Entity pawn, StateHasher& h) const;

private:
    void emitAction(Entity pawn, int zone, int steps, const char* message, int count);

    Things& things_;
    HealthSystem& health_;
    FighterSystem& fighters_;
    const GameTime& time_;
    PatchRules rules_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Entity> scratch_;
};

}  // namespace colony

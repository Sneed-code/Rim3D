#pragma once

#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Health.h"
#include "ColonySim/Wounds.h"

#include <unordered_map>
#include <vector>

namespace colony {

// Turns a pawn's Health into the WoundViews of a snapshot (task V7, docs/design/wounds.md). It keeps what it works out
// about each body (which part is on which skin region, how much it bleeds) and about each Hediffs def, so a snapshot
// costs a loop over the injuries.
class WoundViews {
public:
    WoundViews(const DefDatabase& defs, const HealthSystem& health);

    // Adds the pawn's visible wounds to `out`: its injuries in the order they happened, then its lost parts. Injuries on
    // parts inside the body (organs, bones) are left out. `pawn` is the entity and `pawnDef` its Things def.
    void append(Entity pawn, DefId pawnDef, const Health& health, std::vector<WoundView>& out);

private:
    struct PartData {
        bool inside = false;
        DefId def;
        WoundRegion region = WoundRegion::Torso;
        float bleedRate = 1.0f;
    };
    struct HediffData {
        bool known = false;
        WoundKind kind = WoundKind::Cut;
        float bleedRate = 0.0f;
    };

    const DefDatabase& defs_;
    const HealthSystem& health_;
    std::unordered_map<uint32_t, std::vector<PartData>> bodies_;  // by Things def id
    std::vector<HediffData> hediffs_;                              // by DefId value

    const std::vector<PartData>& body(DefId pawnDef);
    const HediffData& hediff(DefId def);
    // The disease is past its first, unnoticed stage.
    bool diseaseShows(const Disease& disease) const;
};

}  // namespace colony

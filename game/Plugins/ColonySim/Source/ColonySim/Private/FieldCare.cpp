#include "ColonySim/FieldCare.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float careDist(float ax, float ay, float bx, float by) {
    return std::hypot(ax - bx, ay - by);
}

float careCentre(int v) {
    return static_cast<float>(v) + 0.5f;
}

// TendPatient's steps before the work itself (fetch medicine, pick it up, walk to the patient), Rescue's walk to the
// downed: until then a duty job is dropped if its target comes under fire. MedicineJobs.cpp has the steps.
constexpr uint8_t kCareTendWorkStep = 3;
constexpr uint8_t kCareRescueLiftStep = 1;

void careReadRules(const DefDatabase& defs, FieldCareRules& r) {
    const Def* def = defs.find("Rules", "FieldCareRules");
    if (def == nullptr) {
        return;
    }
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(def->number(key, out)); };
    const auto whole = [&](const char* key, int& out) { out = static_cast<int>(def->integer(key, out)); };
    whole("thinkEvery", r.thinkEvery);
    num("exposureRange", r.exposureRange);
    num("keepAway", r.keepAway);
    num("rallyBehind", r.rallyBehind);
    whole("rallySearch", r.rallySearch);
    num("selfDefenseRadius", r.selfDefenseRadius);
    num("dragRadius", r.dragRadius);
    num("dragEnemyRadius", r.dragEnemyRadius);
    whole("dragMaxExposure", r.dragMaxExposure);
    whole("dragDistance", r.dragDistance);
    num("routeStep", r.routeStep);
    r.thinkEvery = std::max(1, r.thinkEvery);
    r.rallySearch = std::max(0, r.rallySearch);
    r.dragDistance = std::max(1, r.dragDistance);
    r.routeStep = std::max(0.25f, r.routeStep);
}

}  // namespace

FieldCare::FieldCare(Things& things, Map& map, HealthSystem& health, JobSystem& jobs, FighterSystem& fighters, RaidSystem& raids,
                     const DefDatabase& defs, const GameTime& time)
    : things_(things), map_(map), health_(health), jobs_(jobs), fighters_(fighters), raids_(raids), time_(time) {
    careReadRules(defs, rules_);
    doctorWork_ = jobs_.workTypeIndex(defs.idOf("WorkTypes", "Doctor"));
    firefighterWork_ = jobs_.workTypeIndex(defs.idOf("WorkTypes", "Firefighter"));
}

int64_t FieldCare::now() const {
    return time_.seconds;
}

void FieldCare::invalidate() {
    hostilesAt_ = -1;
}

// ---- What the enemies see -------------------------------------------------------------------------

// The colony's enemies that can still fight: the alarm's own rule (Raids.cpp, thinkAlarm).
void FieldCare::refreshHostiles() {
    if (hostilesAt_ == now()) {
        return;
    }
    hostilesAt_ = now();
    hostiles_.clear();
    const entt::registry& reg = things_.registry();
    for (const auto [pawn, faction, pos] : reg.view<const Faction, const Position>().each()) {
        const Animal* wild = reg.try_get<Animal>(pawn);
        if (faction.id == kPlayerFaction || (wild != nullptr && wild->temper == AnimalTemper::Calm) || !things_.spawned(pawn) ||
            health_.incapacitated(pawn)) {
            continue;
        }
        const Fighter* f = reg.try_get<Fighter>(pawn);
        if (f != nullptr && f->mode == FightMode::Flee) {
            continue;
        }
        hostiles_.push_back(Hostile{pawn, pos.x, pos.y, pos.level});
    }
    std::sort(hostiles_.begin(), hostiles_.end(), [](const Hostile& a, const Hostile& b) { return a.pawn < b.pawn; });
}

int FieldCare::exposure(float x, float y, int level) {
    refreshHostiles();
    int seen = 0;
    for (const Hostile& h : hostiles_) {
        if (h.level != level || careDist(h.x, h.y, x, y) > rules_.exposureRange) {
            continue;
        }
        if (map_.cover().lineOfSight(Viewpoint{h.x, h.y, h.level, Stance::Standing}, Viewpoint{x, y, level, Stance::Standing})) {
            ++seen;
        }
    }
    return seen;
}

bool FieldCare::safe(CellPos cell) {
    refreshHostiles();
    const float x = careCentre(cell.x);
    const float y = careCentre(cell.y);
    for (const Hostile& h : hostiles_) {
        if (h.level == cell.level && careDist(h.x, h.y, x, y) <= rules_.keepAway) {
            return false;
        }
    }
    return exposure(x, y, cell.level) == 0;
}

// ---- Duty -------------------------------------------------------------------------------------------

int FieldCare::dutyWorkType(Entity pawn) const {
    if (!raids_.state().alarm) {
        return -1;
    }
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Colonist, Position>(pawn) || reg.any_of<Fighter, PlayerControl>(pawn) ||
        fighters_.factionOf(pawn) != kPlayerFaction || health_.incapacitated(pawn)) {
        return -1;
    }
    switch (raids_.roleOf(pawn)) {
        case DefenseRole::Medic: return doctorWork_;
        case DefenseRole::Firefighter: return firefighterWork_;
        default: return -1;
    }
}

bool FieldCare::dutyAllows(Entity pawn, const JobTarget& target, CellPos cell) {
    const entt::registry& reg = things_.registry();
    if (target.thing != kNoEntity && target.thing != pawn) {
        if (rescuerOf(reg, target.thing) != kNoEntity) {
            return false;  // a fighter is bringing it in
        }
        if (reg.all_of<Fighter>(target.thing) && !health_.incapacitated(target.thing)) {
            return false;  // still on its feet in the fight: a medic doesn't chase it about
        }
    }
    return safe(cell);
}

CellPos FieldCare::rallyPoint(Entity pawn) {
    const Position& pos = things_.registry().get<Position>(pawn);
    const CellPos here = cellOf(pos);
    const RaidState& state = raids_.state();
    if (!state.hasHome) {
        return here;
    }
    float px = state.homeX;
    float py = state.homeY;
    if (state.hasThreat) {
        const float dx = state.homeX - state.threatX;
        const float dy = state.homeY - state.threatY;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > 0.0f) {
            px += dx / d * rules_.rallyBehind;
            py += dy / d * rules_.rallyBehind;
        }
    }
    // Each waits in a cell of its own.
    std::vector<CellPos> taken;
    for (const auto [other, worker] : things_.registry().view<const Worker>().each()) {
        if (other != pawn && worker.job.type == JobType::StandBy) {
            taken.push_back(worker.job.cell);
        }
    }
    // Rings out from the point: the first cell out of sight it can reach, else the first it can reach at all.
    const CellPos c0{static_cast<int>(std::floor(px)), static_cast<int>(std::floor(py)), pos.level};
    bool anyFound = false;
    CellPos any = here;
    for (int r = 0; r <= rules_.rallySearch; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{c0.x + dx, c0.y + dy, c0.level};
                if (!map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c) || !jobs_.canReach(here, c) ||
                    std::find(taken.begin(), taken.end(), c) != taken.end()) {
                    continue;
                }
                if (safe(c)) {
                    return c;
                }
                if (!anyFound) {
                    anyFound = true;
                    any = c;
                }
            }
        }
    }
    return any;
}

// An on-duty colonist: an enemy close by makes it fight; a job whose target came under fire before it got there is
// dropped (the job system gives it another, or sends it to wait).
void FieldCare::checkDuty(Entity pawn) {
    entt::registry& reg = things_.registry();
    const Position& pos = reg.get<Position>(pawn);
    refreshHostiles();
    for (const Hostile& h : hostiles_) {
        if (h.level != pos.level || careDist(h.x, h.y, pos.x, pos.y) > rules_.selfDefenseRadius) {
            continue;
        }
        if (!map_.cover().lineOfSight(Viewpoint{pos.x, pos.y, pos.level, Stance::Standing},
                                      Viewpoint{h.x, h.y, h.level, Stance::Standing})) {
            continue;
        }
        // As a Keep working colonist does (raids.md): it fights where it is until the alarm stops.
        Fighter& fighter = fighters_.enlist(pawn, "enemies close");
        fighter.defending = true;
        fighter.anchor = cellOf(pos);
        fighter.leash = raids_.rules().defenderLeash;
        return;
    }
    const Worker* worker = reg.try_get<Worker>(pawn);
    if (worker == nullptr) {
        return;
    }
    const Job& job = worker->job;
    bool exposed = false;
    if ((job.type == JobType::TendPatient && job.step < kCareTendWorkStep && job.thing != pawn) ||
        (job.type == JobType::Rescue && job.step < kCareRescueLiftStep)) {
        exposed = things_.spawned(job.thing) && !safe(cellOf(reg.get<Position>(job.thing)));
    } else if (job.type == JobType::ExtinguishFire) {
        exposed = !safe(job.cell);
    } else if (job.type == JobType::StandBy) {
        // Its place came into sight (the threat moved): it moves if there is a better one.
        if (!safe(job.cell) && rallyPoint(pawn) != job.cell) {
            jobs_.interrupt(pawn, "moving back");
        }
        return;
    }
    if (exposed) {
        ++givenUp_;
        jobs_.interrupt(pawn, "under fire");
    }
}

void FieldCare::step() {
    if (!raids_.state().alarm || now() % rules_.thinkEvery != 0) {
        return;
    }
    const entt::registry& reg = things_.registry();
    std::vector<Entity> onDuty;
    for (const auto [pawn, colonist] : reg.view<const Colonist>().each()) {
        (void)colonist;
        if (dutyWorkType(pawn) >= 0) {
            onDuty.push_back(pawn);
        }
    }
    std::sort(onDuty.begin(), onDuty.end());
    for (const Entity pawn : onDuty) {
        checkDuty(pawn);
    }
    considerDrags();
}

// ---- Dragging the downed to cover ---------------------------------------------------------------

bool FieldCare::routeSafeEnough(float ax, float ay, float bx, float by, int level) {
    const float d = careDist(ax, ay, bx, by);
    const int points = std::max(1, static_cast<int>(std::ceil(d / rules_.routeStep)));
    for (int i = 0; i <= points; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(points);
        if (exposure(ax + (bx - ax) * t, ay + (by - ay) * t, level) > rules_.dragMaxExposure) {
            return false;
        }
    }
    return true;
}

// The cover a downed pawn is dragged to: in the nearest ring round it that has any, the cell out of the enemies'
// sight nearest home (the first in ring order with no home), walkable and free, that it can be carried to.
bool FieldCare::dropCell(Entity patient, CellPos& out) {
    const Position& at = things_.registry().get<Position>(patient);
    const CellPos from = cellOf(at);
    const RaidState& state = raids_.state();
    for (int r = 1; r <= rules_.dragDistance; ++r) {
        bool found = false;
        float best = 0.0f;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{from.x + dx, from.y + dy, from.level};
                if (!map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c) || jobs_.bedOccupied(c, patient) ||
                    !jobs_.canReach(from, c) || !safe(c)) {
                    continue;
                }
                const float score = state.hasHome ? careDist(careCentre(c.x), careCentre(c.y), state.homeX, state.homeY) : 0.0f;
                if (!found || score < best) {
                    found = true;
                    best = score;
                    out = c;
                }
            }
        }
        if (found) {
            return true;
        }
    }
    return false;
}

void FieldCare::considerDrags() {
    refreshHostiles();
    if (hostiles_.empty()) {
        return;
    }
    entt::registry& reg = things_.registry();
    // The downed of the colony lying where no bed holds them, nobody is coming for and the enemies can see.
    std::vector<Entity> downed;
    for (const auto [pawn, health, pos] : reg.view<const Health, const Position>().each()) {
        if (health.downed && !health.dead && reg.all_of<Colonist>(pawn) && fighters_.factionOf(pawn) == kPlayerFaction &&
            things_.spawned(pawn) && jobs_.needsRescue(pawn) && jobs_.reservedBy(JobTarget{pawn, CellPos{}}) == kNoEntity) {
            downed.push_back(pawn);
        }
    }
    std::sort(downed.begin(), downed.end());
    // The alarm's fighters that could go: fighting, not the player's, not carrying anyone.
    std::vector<Entity> fighters;
    for (const auto [pawn, f] : reg.view<const Fighter>().each()) {
        if (f.defending && !f.drafted && f.mode == FightMode::Fight && things_.spawned(pawn) && !health_.incapacitated(pawn) &&
            !reg.all_of<PlayerControl>(pawn) && fighters_.factionOf(pawn) == kPlayerFaction) {
            fighters.push_back(pawn);
        }
    }
    std::sort(fighters.begin(), fighters.end());
    for (const Entity patient : downed) {
        if (fighters.empty()) {
            return;
        }
        const Position& at = reg.get<Position>(patient);
        const CellPos cell = cellOf(at);
        if (safe(cell)) {
            continue;  // out of sight already: the medics can reach it
        }
        const bool enemyClose = std::any_of(hostiles_.begin(), hostiles_.end(), [&](const Hostile& h) {
            return h.level == at.level && careDist(h.x, h.y, at.x, at.y) <= rules_.dragEnemyRadius;
        });
        if (enemyClose || exposure(careCentre(cell.x), careCentre(cell.y), cell.level) > rules_.dragMaxExposure) {
            continue;
        }
        // The nearest fighter in range.
        size_t pick = fighters.size();
        float pickDist = 0.0f;
        for (size_t i = 0; i < fighters.size(); ++i) {
            const Position& p = reg.get<Position>(fighters[i]);
            const float d = careDist(p.x, p.y, at.x, at.y);
            if (p.level == at.level && d <= rules_.dragRadius && (pick == fighters.size() || d < pickDist)) {
                pick = i;
                pickDist = d;
            }
        }
        if (pick == fighters.size()) {
            continue;
        }
        CellPos to;
        if (!dropCell(patient, to)) {
            continue;
        }
        const Position& p = reg.get<Position>(fighters[pick]);
        if (!routeSafeEnough(p.x, p.y, at.x, at.y, at.level) ||
            !routeSafeEnough(at.x, at.y, careCentre(to.x), careCentre(to.y), at.level)) {
            continue;
        }
        if (fighters_.startRescue(fighters[pick], patient, to)) {
            ++drags_;
            fighters.erase(fighters.begin() + static_cast<std::ptrdiff_t>(pick));
        }
    }
}

}  // namespace colony

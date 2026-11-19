// Earthworks (docs/design/earthworks.md): pawns dig or fill ground planned flat, one unit (a corner
// Map::kEarthUnitM up or down) at a time. Ground under a building plan is dug by its builder as the first
// part of building (JobSystem::buildWork); everything else (between walls, rooms, the Level earth order)
// by the Earthworks job here, in the Construction work type. Part of the job system.
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

// The four cells round a corner (cells x-1..x, y-1..y share corner x, y).
template <typename F>
void earthJobsCellsRound(const Map& map, int corner, F&& each) {
    const CellPos k = map.cellAt(corner);
    for (int y = k.y - 1; y <= k.y; ++y) {
        for (int x = k.x - 1; x <= k.x; ++x) {
            const CellPos c{x, y, 0};
            if (map.inBounds(c)) {
                each(c);
            }
        }
    }
}

bool earthJobsTreeAt(const Plants& plants, const DefDatabase& defs, CellPos c) {
    const DefId plant = plants.defAt(c);
    return plant.valid() && defs.get(plant).flag("plant.isTree");
}

class EarthworksGiver final : public WorkGiver {
public:
    const char* task() const override { return "Earthworks"; }

    void scan(JobSystem& jobs, Entity pawn, std::vector<WorkCandidate>& out) override {
        const Map& map = jobs.map();
        if (!jobs.canUseHands(pawn) || map.earthCorners().empty()) {
            return;
        }
        const Position& at = jobs.things().registry().get<Position>(pawn);
        if (at.level != 0) {
            return;
        }
        lastCell_ = -1;
        for (const uint32_t corner : map.earthCorners()) {
            if (!jobs.earthCornerFree(static_cast<int>(corner))) {
                continue;
            }
            // Stand on the walkable cell round the corner nearest the pawn; its corners are what the job digs.
            CellPos best{};
            float bestDist = 1.0e30f;
            earthJobsCellsRound(map, static_cast<int>(corner), [&](CellPos c) {
                if (!map.walkable(c)) {
                    return;
                }
                const float dx = static_cast<float>(c.x) + 0.5f - at.x;
                const float dy = static_cast<float>(c.y) + 0.5f - at.y;
                if (dx * dx + dy * dy < bestDist) {
                    bestDist = dx * dx + dy * dy;
                    best = c;
                }
            });
            if (bestDist >= 1.0e30f || map.index(best) == lastCell_) {
                continue;
            }
            lastCell_ = map.index(best);
            out.push_back(WorkCandidate{JobTarget{kNoEntity, best}, bestDist, 0, 0, false, CellPos{}});
        }
    }

    bool makeJob(JobSystem&, Entity, const WorkCandidate& candidate, Job& out) override {
        out = Job{};
        out.type = JobType::Earthworks;
        out.cell = candidate.target.cell;
        return true;
    }

private:
    int lastCell_ = -1;
};

}  // namespace

void addEarthWorkGivers(std::vector<std::unique_ptr<WorkGiver>>& givers) {
    givers.push_back(std::make_unique<EarthworksGiver>());
}

bool JobSystem::earthCornerFree(int corner) const {
    // Ground under a building plan waits for its builder; a tree must be cut first (markEarthTrees marks it).
    bool free = true;
    earthJobsCellsRound(map_, corner, [&](CellPos c) {
        if (construction_.siteAt(c) != kNoEntity || earthJobsTreeAt(plants_, defs_, c)) {
            free = false;
        }
    });
    return free;
}

void JobSystem::earthMoved(int corner, bool clearAll) {
    // The ground changed under whatever grew there: grass and bushes go; trees too where a building digs.
    earthJobsCellsRound(map_, corner, [&](CellPos c) {
        if (plants_.has(c) && (clearAll || !earthJobsTreeAt(plants_, defs_, c))) {
            plants_.remove(c);
        }
    });
}

void JobSystem::markEarthTrees() {
    if (map_.earthChanges() == earthMarked_) {
        return;
    }
    earthMarked_ = map_.earthChanges();
    bool changed = false;
    for (const uint32_t corner : map_.earthCorners()) {
        bool underPlan = false;
        earthJobsCellsRound(map_, static_cast<int>(corner), [&](CellPos c) { underPlan = underPlan || construction_.siteAt(c) != kNoEntity; });
        if (underPlan) {
            continue;  // the builder clears its own footprint
        }
        earthJobsCellsRound(map_, static_cast<int>(corner), [&](CellPos c) {
            if (earthJobsTreeAt(plants_, defs_, c) && designations_.at(c) == DesignationKind::None) {
                designations_.set(c, DesignationKind::CutPlant);
                changed = true;
            }
        });
    }
    if (changed) {
        Event event;
        event.type = EventType::DesignationsChanged;
        events_.push_back(std::move(event));
    }
}

bool JobSystem::failCheckEarth(Entity pawn, Job& job, std::string& why) {
    if (!canUseHands(pawn)) {
        why = "it can't use its hands";
        return false;
    }
    // Earthworks done meanwhile (a neighbour's job or a builder moved the shared corners) are no failure: the work
    // step finds nothing left and ends the job as done.
    (void)job;
    return true;
}

// One unit of earthworks for every earthWorkPerUnit of work, at construction speed, until the cell's
// corners are where they were planned (those under a tree or a building plan wait).
JobSystem::StepResult JobSystem::earthWork(Entity pawn, Job& job, std::string&) {
    if (job.walk == 0) {
        job.walk = 2;  // work steps don't walk: this marks that the animation event went out
        gatherEvent(pawn, PawnAction::Build, job.cell);
    }
    job.progress += constructionSpeed(pawn) * static_cast<float>(construction_.numbers().buildWorkPerSecond);
    learnSkill(pawn, "Construction", gather_.constructionXpPerSecond * gather_.constructionSpeed);
    const float perUnit = static_cast<float>(std::max(construction_.numbers().earthWorkPerUnit, 1.0));
    const auto allowed = [this](int corner) { return earthCornerFree(corner); };
    while (job.progress >= perUnit) {
        job.progress -= perUnit;
        const int corner = map_.digEarth(job.cell, allowed);
        if (corner < 0) {
            return StepResult::Next;  // what is left waits for a tree to be cut or a builder
        }
        earthMoved(corner, false);
    }
    // Done when none of the cell's corners that may be worked now has a target left.
    const int width = map_.width();
    const int height = map_.height();
    for (int cy = job.cell.y; cy <= std::min(job.cell.y + 1, height - 1); ++cy) {
        for (int cx = job.cell.x; cx <= std::min(job.cell.x + 1, width - 1); ++cx) {
            const int corner = cy * width + cx;
            if (!std::isnan(map_.earthTarget(corner)) && earthCornerFree(corner)) {
                return StepResult::Running;
            }
        }
    }
    return StepResult::Next;
}

}  // namespace colony

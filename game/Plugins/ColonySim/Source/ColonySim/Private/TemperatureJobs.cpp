// Colonists seek a safe temperature (docs/design/temperature.md): RimWorld's SeekSafeTemperature.
#include "ColonySim/Jobs.h"

#include "ColonySim/Map.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr int kTempJobsSearchRadius = 30;      // cells looked through for a place in the comfortable range
constexpr int64_t kTempJobsLookEvery = 30;     // a pawn looks this often (game seconds), spread by pawn
constexpr int64_t kTempJobsStaySeconds = 900;  // how long it stays once there: a quarter of a game hour (ours)
constexpr int kTempJobsReachChecks = 8;        // nearest cells tried for a path

}  // namespace

// Outside its safe range (the comfortable range widened by `safeMarginC`) a pawn looks for the nearest cell in its
// comfortable range that it can reach, and goes there. Nowhere to go (a hard frost over the whole map): it carries on.
bool JobSystem::thinkTemperature(Entity pawn, Job& out) {
    if (temperature_ == nullptr || (now() + static_cast<int64_t>(entt::to_integral(pawn))) % kTempJobsLookEvery != 0) {
        return false;
    }
    float comfyMin = 0.0f;
    float comfyMax = 0.0f;
    if (!temperature_->comfortRange(pawn, comfyMin, comfyMax)) {
        return false;
    }
    const float margin = temperature_->numbers().safeMarginC;
    const float here = temperature_->ambientOf(pawn);
    if (here >= comfyMin - margin && here <= comfyMax + margin) {
        return false;
    }
    const CellPos from = cellOf(things_.registry().get<Position>(pawn));
    std::vector<std::pair<int, CellPos>> found;
    for (int dy = -kTempJobsSearchRadius; dy <= kTempJobsSearchRadius; ++dy) {
        for (int dx = -kTempJobsSearchRadius; dx <= kTempJobsSearchRadius; ++dx) {
            const CellPos c{from.x + dx, from.y + dy, from.level};
            if (!map_.inBounds(c) || !map_.walkable(c) || map_.isDoor(c)) {
                continue;
            }
            const float t = temperature_->temperatureAt(c);
            if (t >= comfyMin && t <= comfyMax) {
                found.emplace_back(dx * dx + dy * dy, c);
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first
                                  : (a.second.y != b.second.y ? a.second.y < b.second.y : a.second.x < b.second.x);
    });
    int checks = 0;
    for (const auto& [dist, cell] : found) {
        (void)dist;
        if (avoiding(pawn, JobTarget{kNoEntity, cell})) {
            continue;
        }
        if (canReach(from, cell)) {
            out = Job{};
            out.type = JobType::SeekTemperature;
            out.cell = cell;
            out.until = now() + kTempJobsStaySeconds;
            return true;
        }
        if (++checks >= kTempJobsReachChecks) {
            break;
        }
    }
    return false;
}

// A pawn on a job it may drop (idling, hauling, joy, cleaning...) that is out of its safe range takes the same walk.
void JobSystem::interruptForTemperature(Entity pawn) {
    const Job& current = things_.registry().get<Worker>(pawn).job;
    if (temperature_ == nullptr || current.forced || current.handsOn != 0 || current.type == JobType::SeekTemperature ||
        (current.type != JobType::Wait && current.type != JobType::HaulToCell && current.type != JobType::Recreate &&
         current.type != JobType::Research && current.type != JobType::CleanFilth && !jobIsGather(current.type))) {
        return;
    }
    Job job;
    if (!thinkTemperature(pawn, job)) {
        return;
    }
    endJob(pawn, JobEnd::Interrupted, "too cold or hot here");
    startJob(pawn, std::move(job));
}

}  // namespace colony

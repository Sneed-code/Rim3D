// Lovin (docs/design/lovin.md): partners who lie down in the same bed now and then have lovin. It is abstract: the two
// stay lying in their bed a while (Unreal shows them under the covers with a hearts icon) and afterwards each is
// happier and thinks a little better of the other. Adults only, always.

#include "ColonySim/Social.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Protagonist.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

constexpr uint64_t kLovinSaltLength = 0x4C4F56494E4C454EULL;
constexpr uint64_t kLovinSaltGapA = 0x4C4F56494E474150ULL;
constexpr uint64_t kLovinSaltGapB = 0x4C4F56494E474142ULL;
constexpr uint64_t kLovinSaltPose = 0x4C4F56494E504F53ULL;

}  // namespace

const char* lovinPoseName(LovinPose pose) {
    switch (pose) {
        case LovinPose::SideBySide: return "SideBySide";
        case LovinPose::Spooning: return "Spooning";
        case LovinPose::Embracing: return "Embracing";
    }
    return "?";
}

bool SocialSystem::inLovin(Entity pawn) const {
    return things_.registry().valid(pawn) && things_.registry().all_of<Lovin>(pawn);
}

float SocialSystem::lovinIntervalHours(Entity pawn) const {
    const auto& curve = rules_.lovinIntervalHours;
    if (curve.empty()) {
        return rules_.lovinIntervalMin;
    }
    const Colonist* colonist = things_.registry().valid(pawn) ? things_.registry().try_get<Colonist>(pawn) : nullptr;
    const float age = colonist != nullptr ? static_cast<float>(colonist->age) : 30.0f;
    if (age <= curve.front().first) {
        return curve.front().second;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (age <= curve[i].first) {
            const float span = curve[i].first - curve[i - 1].first;
            const float t = span > 0.0f ? (age - curve[i - 1].first) / span : 1.0f;
            return curve[i - 1].second + t * (curve[i].second - curve[i - 1].second);
        }
    }
    return curve.back().second;
}

// The bed the pawn lies in now (asleep or about to be), or none; `layDownAt` the game second it lay down.
Entity SocialSystem::lovinBed(Entity pawn, int64_t& layDownAt) const {
    const Worker* worker = things_.registry().try_get<Worker>(pawn);
    if (worker == nullptr || !jobIsLyingDown(worker->job) || worker->job.thing == kNoEntity || worker->job.until == 0) {
        return kNoEntity;
    }
    layDownAt = worker->job.until - 1;  // the sleep job marks the moment it lay down with until = then + 1
    return worker->job.thing;
}

// Free for lovin: a living adult of the colony on the map, on its feet, not in a break, a fight or under the player's
// hand, not asexual, and past its pause since the last time.
bool SocialSystem::lovinReady(Entity pawn, int64_t now) const {
    const entt::registry& reg = things_.registry();
    if (!takesPart(pawn) || !things_.spawned(pawn) || health_.incapacitated(pawn) || !isAdult(pawn)) {
        return false;
    }
    // A pawn without a Colonist record counts as an adult elsewhere; lovin asks for a real age.
    const Colonist* colonist = reg.try_get<Colonist>(pawn);
    if (colonist == nullptr || colonist->age < rules_.adultAge) {
        return false;
    }
    if (reg.all_of<Lovin>(pawn) || reg.all_of<SocialFight>(pawn) || reg.all_of<PlayerControl>(pawn)) {
        return false;
    }
    if (const MentalBreaker* breaker = reg.try_get<MentalBreaker>(pawn); breaker != nullptr && breaker->active) {
        return false;
    }
    if (orientationOf(pawn) == Orientation::Asexual) {
        return false;
    }
    return reg.get<Social>(pawn).nextLovin <= now;
}

void SocialSystem::endLovin(Entity a, Entity b, bool done) {
    entt::registry& reg = things_.registry();
    for (const Entity pawn : {a, b}) {
        if (reg.valid(pawn)) {
            reg.remove<Lovin>(pawn);
        }
    }
    if (done) {
        const int64_t now = time_.seconds;
        int index = 0;
        for (const auto& [pawn, other] : {std::pair<Entity, Entity>{a, b}, std::pair<Entity, Entity>{b, a}}) {
            if (!reg.valid(pawn) || !reg.all_of<Social>(pawn)) {
                continue;
            }
            thought(pawn, rules_.lovinThought);
            addMemory(pawn, other, rules_.lovinLabel, rules_.lovinOpinion, rules_.lovinOpinionDays, 1);
            // The pause before the next: the curve over its age with a spread (Box-Muller on two hash rolls).
            const double u1 = std::max(1.0e-9, unit(pawn, index == 0 ? kLovinSaltGapA : kLovinSaltGapB));
            const double u2 = unit(other, index == 0 ? kLovinSaltGapB : kLovinSaltGapA);
            const double normal = std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
            const double hours = std::max(static_cast<double>(rules_.lovinIntervalMin),
                                          static_cast<double>(lovinIntervalHours(pawn)) + normal * rules_.lovinIntervalSpread);
            reg.get<Social>(pawn).nextLovin = now + static_cast<int64_t>(std::llround(hours * static_cast<double>(kSecondsPerHour)));
            ++index;
        }
        ++stats_.lovins;
    }
    push(EventType::Lovin, a, b, done ? "done" : "interrupted", done ? 1 : 0, 0.0f);
}

void SocialSystem::stepLovin(int64_t now, const std::vector<Entity>& pawns) {
    entt::registry& reg = things_.registry();

    // Lovin under way: it ends when its time is up, or early if either gets up, leaves the bed, is hurt or is gone.
    for (const Entity pawn : pawns) {
        const Lovin* lovin = reg.try_get<Lovin>(pawn);
        if (lovin == nullptr) {
            continue;
        }
        const Entity other = lovin->partner;
        const Lovin* theirs = reg.valid(other) ? reg.try_get<Lovin>(other) : nullptr;
        if (theirs == nullptr || theirs->partner != pawn) {
            endLovin(pawn, other, false);
            continue;
        }
        if (other < pawn) {
            continue;  // the pair is looked at once, from its lower id
        }
        if (now >= lovin->until) {
            endLovin(pawn, other, true);
            continue;
        }
        int64_t downA = 0;
        int64_t downB = 0;
        const Entity bed = lovinBed(pawn, downA);
        const bool still = bed != kNoEntity && bed == lovinBed(other, downB) && things_.spawned(pawn) && things_.spawned(other) &&
                           !health_.incapacitated(pawn) && !health_.incapacitated(other) && !reg.all_of<PlayerControl>(pawn) &&
                           !reg.all_of<PlayerControl>(other);
        if (!still) {
            endLovin(pawn, other, false);
        }
    }

    // A couple newly lying in one bed, both free for it: lovin starts.
    for (const Entity pawn : pawns) {
        const Entity other = partner(pawn);
        if (other == kNoEntity || other < pawn || !reg.all_of<Social>(other)) {
            continue;
        }
        int64_t downA = 0;
        int64_t downB = 0;
        const Entity bed = lovinBed(pawn, downA);
        if (bed == kNoEntity || bed != lovinBed(other, downB)) {
            continue;
        }
        const int64_t window = static_cast<int64_t>(std::llround(static_cast<double>(rules_.lovinWindowMinutes) * 60.0));
        if (now - std::max(downA, downB) > window || !lovinReady(pawn, now) || !lovinReady(other, now)) {
            continue;
        }
        const double share = unit(pawn, kLovinSaltLength);
        const double minutes = rules_.lovinMinMinutes + share * (rules_.lovinMaxMinutes - rules_.lovinMinMinutes);
        const int64_t until = now + std::max<int64_t>(60, static_cast<int64_t>(std::llround(minutes * 60.0)));
        const LovinPose pose = static_cast<LovinPose>(std::min(2, static_cast<int>(unit(pawn, kLovinSaltPose) * 3.0)));
        reg.emplace_or_replace<Lovin>(pawn, Lovin{other, now, until, pose});
        reg.emplace_or_replace<Lovin>(other, Lovin{pawn, now, until, pose});
        push(EventType::Lovin, pawn, other, "started", 1, static_cast<float>(until - now));
    }
}

}  // namespace colony

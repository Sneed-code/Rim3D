#include "MovementSystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Genes.h"
#include "ColonySim/Health.h"
#include "ColonySim/Lights.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Random.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr float kMoveBucketSize = 4.0f;           // metres; also how far a pawn looks for neighbours
constexpr int kMoveMaxNeighbours = 8;             // the nearest this many are steered around
constexpr float kMoveAccelFraction = 0.2f;        // of the pawn's speed, per step
constexpr float kMoveReachWaypoint = 0.2f;        // metres from a waypoint that counts as there (a cell
                                                  // centre is 0.5 m from the walls, a pawn needs 0.3)
constexpr float kMoveLookAhead = 1.5f;            // metres ahead on its line a pawn aims for
constexpr float kMoveCornerSpeed = 0.35f;         // share of its speed kept through a right-angle turn
constexpr int kMoveGiveUpAfter = 300;             // steps held up before the pawn drops its path
constexpr float kMoveReachGoal = 0.25f;           // the same for the last one
constexpr int kMoveArrivedBlockedSteps = 10;      // held up this long inside the goal cell: arrived
constexpr float kMoveHorizon = 30.0f;             // steps ahead a meeting is looked for
constexpr float kMoveMargin = 0.1f;               // metres of room kept between pawns
constexpr float kMoveCollisionWeight = 3.0f;
constexpr float kMoveSpace = 0.35f;               // metres beyond touching that pawns like to keep
constexpr float kMoveSpaceWeight = 2.0f;
constexpr float kMoveRoomyReach = 0.8f;           // metres round a pawn that must be clear of walls to skip the wall tests
constexpr float kMoveWallPenalty = 100.0f;
constexpr float kMoveTurnPenalty = 0.02f;         // per 22.5 degrees turned, to settle on one side
constexpr float kMoveLeftExtra = 0.03f;           // and a little more for turning left
constexpr float kMoveSeparationSpeed = 0.5f;      // of the pawn's speed, pushing out of a pile
constexpr float kMoveHeldBackShare = 0.3f;        // under this share of the wanted speed counts as held back
constexpr int kMoveGhostAfter = 45;               // steps held back before walking through the others
constexpr int kMoveGhostSteps = 30;               // and for how many steps
constexpr float kMoveDoorLookAhead = 8.0f;        // steps ahead the pawn the player walks asks a door to open

// Unit vectors every 22.5 degrees (cos, sin), counter-clockwise on a map with y up. Written out
// so no platform's cos() and sin() can make two machines disagree.
constexpr float kMoveDirs[16][2] = {
    {1.0f, 0.0f},         {0.9238795f, 0.3826834f},   {0.7071068f, 0.7071068f},   {0.3826834f, 0.9238795f},
    {0.0f, 1.0f},         {-0.3826834f, 0.9238795f},  {-0.7071068f, 0.7071068f},  {-0.9238795f, 0.3826834f},
    {-1.0f, 0.0f},        {-0.9238795f, -0.3826834f}, {-0.7071068f, -0.7071068f}, {-0.3826834f, -0.9238795f},
    {0.0f, -1.0f},        {0.3826834f, -0.9238795f},  {0.7071068f, -0.7071068f},  {0.9238795f, -0.3826834f}};

struct MoveNeighbour {
    float dist2 = 0.0f;
    Entity entity = kNoEntity;
    float px = 0.0f;  // where it is, relative to this pawn
    float py = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
};

bool moveCloser(const MoveNeighbour& a, const MoveNeighbour& b) {
    return a.dist2 < b.dist2 || (a.dist2 == b.dist2 && a.entity < b.entity);
}

// Steps until two pawns of combined radius `reach` meet, one at `p` from the other and moving at
// `v` relative to it; a large number if they don't. Already touching and closing counts as now.
float moveTimeToMeet(float px, float py, float vx, float vy, float reach) {
    const float dist2 = px * px + py * py;
    const float closing = px * vx + py * vy;
    if (closing <= 0.0f) {
        return 1.0e9f;  // moving apart or alongside
    }
    if (dist2 <= reach * reach) {
        return 0.0f;
    }
    const float a = vx * vx + vy * vy;
    const float disc = closing * closing - a * (dist2 - reach * reach);
    if (disc <= 0.0f) {
        return 1.0e9f;  // passes by
    }
    return (closing - std::sqrt(disc)) / a;
}

}  // namespace

void MovementSystem::step(entt::registry& reg, Map& map) {
    doorRequests_.clear();
    movePawns(reg, map);
    updateDoors(reg, map);
}

// Each door opens while some pawn needs it and closes `closeDelaySteps` after the last one did.
void MovementSystem::updateDoors(entt::registry& reg, Map& map) {
    std::sort(doorRequests_.begin(), doorRequests_.end());
    for (const auto [entity, door] : reg.view<Door>().each()) {
        const bool wasOpen = door.open;
        if (std::binary_search(doorRequests_.begin(), doorRequests_.end(), entity)) {
            if (door.open) {
                door.hold = static_cast<uint16_t>(doors_.closeDelaySteps);
            } else if (++door.progress >= doors_.openSteps) {
                door.open = true;
                door.hold = static_cast<uint16_t>(doors_.closeDelaySteps);
            }
        } else if (door.open) {
            if (door.hold > 0) {
                --door.hold;
            }
            if (door.hold == 0) {
                door.open = false;
                door.progress = 0;
            }
        } else {
            door.progress = 0;
        }
        if (door.open != wasOpen) {
            setDoorSight(map, reg.get<Footprint>(entity), door.open);
        }
    }
}

void MovementSystem::movePawns(entt::registry& reg, const Map& map) {
    heldUp_ = 0;
    agents_.clear();
    for (const auto [entity, pos, mover] : reg.view<const Position, const Mover>().each()) {
        Agent agent;
        agent.entity = entity;
        agent.x = pos.x;
        agent.y = pos.y;
        agent.level = pos.level;
        agent.vx = mover.vx;
        agent.vy = mover.vy;
        agents_.push_back(agent);
    }
    if (agents_.empty()) {
        return;
    }
    std::sort(agents_.begin(), agents_.end(), [](const Agent& a, const Agent& b) { return a.entity < b.entity; });

    // Buckets of kMoveBucketSize metres, each a list of agents in id order.
    const int bucketsX = static_cast<int>(static_cast<float>(map.width()) / kMoveBucketSize) + 1;
    const int bucketsY = static_cast<int>(static_cast<float>(map.height()) / kMoveBucketSize) + 1;
    const auto bucketCoord = [&](float v, int count) {
        return std::clamp(static_cast<int>(v / kMoveBucketSize), 0, count - 1);
    };
    if (bucketHead_.size() != static_cast<size_t>(bucketsX) * static_cast<size_t>(bucketsY)) {
        bucketHead_.assign(static_cast<size_t>(bucketsX) * static_cast<size_t>(bucketsY), -1);
    }
    nextInBucket_.assign(agents_.size(), -1);
    for (int i = static_cast<int>(agents_.size()) - 1; i >= 0; --i) {
        Agent& a = agents_[static_cast<size_t>(i)];
        a.bucket = bucketCoord(a.y, bucketsY) * bucketsX + bucketCoord(a.x, bucketsX);
        nextInBucket_[static_cast<size_t>(i)] = bucketHead_[static_cast<size_t>(a.bucket)];
        bucketHead_[static_cast<size_t>(a.bucket)] = i;
    }

    const float reach = 2.0f * kPawnRadius + kMoveMargin;
    MoveNeighbour closest[kMoveMaxNeighbours];

    for (size_t index = 0; index < agents_.size(); ++index) {
        Agent& a = agents_[index];
        Mover& mover = reg.get<Mover>(a.entity);
        // A pawn standing in a door keeps it open, even a downed one.
        const CellPos here{static_cast<int>(std::floor(a.x)), static_cast<int>(std::floor(a.y)), a.level};
        if (map.inBounds(here) && map.passage(here) == Passage::Door) {
            requestDoor(map.buildingAt(here));
        }
        if (reg.all_of<PlayerControl>(a.entity)) {
            // The player's pawn: DirectMoveCommand moved it already. The others steer around it. It
            // is never held up by a door, but it opens the one it is walking up to.
            const CellPos ahead{static_cast<int>(std::floor(a.x + a.vx * kMoveDoorLookAhead)),
                                static_cast<int>(std::floor(a.y + a.vy * kMoveDoorLookAhead)), a.level};
            if (map.inBounds(ahead) && map.passage(ahead) == Passage::Door) {
                requestDoor(map.buildingAt(ahead));
            }
            a.newVx = a.vx;
            a.newVy = a.vy;
            continue;
        }
        // Injuries slow a pawn down (its Moving capacity); a downed or dead one doesn't walk.
        const Health* health = reg.try_get<Health>(a.entity);
        const float lightFactor = lights_ != nullptr ? lights_->moveSpeedAt(here) : 1.0f;
        const float weatherFactor = weather_ != nullptr ? weather_->moveFactorAt(here) : 1.0f;
        const PawnGenes* genes = reg.try_get<PawnGenes>(a.entity);
        const float geneSpeed = genes != nullptr ? genes->effects.moveSpeed / static_cast<float>(kStepsPerRealSecondAt1x) : 0.0f;
        const float base = std::max(0.0f, mover.metresPerStep + geneSpeed) * (health != nullptr ? health->movingFactor : 1.0f) *
                           lightFactor * weatherFactor * (speedFactor_ ? speedFactor_(a.entity) : 1.0f);
        if (base <= 0.0f || mover.hold || reg.all_of<KnockedDown>(a.entity)) {
            // Downed or dead, knocked down by a blast, or working standing still: it stays where it is. Others still steer around it.
            a.newVx = 0.0f;
            a.newVy = 0.0f;
            mover.blockedSteps = 0;
            mover.heldSteps = 0;
            continue;
        }
        const float accel = base * kMoveAccelFraction;

        // Where to: skip the waypoints already reached.
        while (!mover.arrived()) {
            const CellPos wp = mover.path[mover.next];
            const float dx = static_cast<float>(wp.x) + 0.5f - a.x;
            const float dy = static_cast<float>(wp.y) + 0.5f - a.y;
            const float d2 = dx * dx + dy * dy;
            const bool last = mover.next + 1 == mover.path.size();
            const float reachDist = last ? kMoveReachGoal : kMoveReachWaypoint;
            bool reached = d2 <= reachDist * reachDist;
            if (last && !reached && mover.blockedSteps >= kMoveArrivedBlockedSteps) {
                reached = static_cast<int>(std::floor(a.x)) == wp.x && static_cast<int>(std::floor(a.y)) == wp.y;
            }
            if (!reached) {
                break;
            }
            mover.fromX = static_cast<float>(wp.x) + 0.5f;
            mover.fromY = static_cast<float>(wp.y) + 0.5f;
            ++mover.next;
            mover.blockedSteps = 0;
        }

        // The velocity it wants and the fastest it may go.
        float wantX = 0.0f;
        float wantY = 0.0f;
        float topSpeed = base * terrainSpeedFactor(map.terrain(CellPos{std::clamp(static_cast<int>(std::floor(a.x)), 0, map.width() - 1),
                                                                        std::clamp(static_cast<int>(std::floor(a.y)), 0, map.height() - 1), 0}));
        const bool walking = !mover.arrived();
        if (walking) {
            // Walk the line from where it started to the waypoint, aiming a little ahead of the
            // closest point on it: a pawn pushed off the line comes back to it, and the line was
            // checked for room, so it never cuts a corner.
            const CellPos wp = mover.path[mover.next];
            const float wx = static_cast<float>(wp.x) + 0.5f;
            const float wy = static_cast<float>(wp.y) + 0.5f;
            const float sx = wx - mover.fromX;
            const float sy = wy - mover.fromY;
            const float segLen2 = sx * sx + sy * sy;
            float aimX = wx;
            float aimY = wy;
            float inX = 0.0f;
            float inY = 0.0f;
            if (segLen2 > 1.0e-6f) {
                const float segLen = std::sqrt(segLen2);
                inX = sx / segLen;
                inY = sy / segLen;
                const float along = ((a.x - mover.fromX) * sx + (a.y - mover.fromY) * sy) / segLen2;
                const float aimAt = std::min(1.0f, std::clamp(along, 0.0f, 1.0f) + kMoveLookAhead / segLen);
                aimX = mover.fromX + sx * aimAt;
                aimY = mover.fromY + sy * aimAt;
            }
            float dx = aimX - a.x;
            float dy = aimY - a.y;
            float aimDist = std::sqrt(dx * dx + dy * dy);
            if (aimDist < 1.0e-4f) {
                dx = wx - a.x;
                dy = wy - a.y;
                aimDist = std::sqrt(dx * dx + dy * dy);
            }
            const float ux = aimDist > 1.0e-6f ? dx / aimDist : 0.0f;
            const float uy = aimDist > 1.0e-6f ? dy / aimDist : 0.0f;
            const float wdx = wx - a.x;
            const float wdy = wy - a.y;
            const float wpDist = std::sqrt(wdx * wdx + wdy * wdy);
            float speed = base * walkSpeedFactor(map, a.x, a.y, ux, uy);
            if (mover.next + 1 == mover.path.size()) {
                speed = std::min(speed, 0.9f * std::sqrt(2.0f * accel * wpDist));  // brake into the goal
            } else {
                // Slow down for a turn at the waypoint: the sharper, the slower.
                const CellPos after = mover.path[mover.next + 1];
                const float ox = static_cast<float>(after.x) + 0.5f - wx;
                const float oy = static_cast<float>(after.y) + 0.5f - wy;
                const float outLen = std::sqrt(ox * ox + oy * oy);
                const float straight = (outLen > 1.0e-6f && segLen2 > 1.0e-6f) ? (inX * ox + inY * oy) / outLen : 1.0f;
                const float keep = kMoveCornerSpeed + (1.0f - kMoveCornerSpeed) * std::max(0.0f, straight);
                speed = std::min(speed, std::sqrt(speed * speed * keep * keep + 2.0f * accel * wpDist));
            }
            topSpeed = speed;
            wantX = ux * speed;
            wantY = uy * speed;
        }

        // The nearest pawns on the same level.
        int count = 0;
        const int bx = a.bucket % bucketsX;
        const int by = a.bucket / bucketsX;
        for (int cy = std::max(by - 1, 0); cy <= std::min(by + 1, bucketsY - 1); ++cy) {
            for (int cx = std::max(bx - 1, 0); cx <= std::min(bx + 1, bucketsX - 1); ++cx) {
                for (int j = bucketHead_[static_cast<size_t>(cy * bucketsX + cx)]; j >= 0; j = nextInBucket_[static_cast<size_t>(j)]) {
                    if (static_cast<size_t>(j) == index) {
                        continue;
                    }
                    const Agent& o = agents_[static_cast<size_t>(j)];
                    if (o.level != a.level) {
                        continue;
                    }
                    MoveNeighbour n;
                    n.px = o.x - a.x;
                    n.py = o.y - a.y;
                    n.dist2 = n.px * n.px + n.py * n.py;
                    if (n.dist2 > kMoveBucketSize * kMoveBucketSize) {
                        continue;
                    }
                    n.entity = o.entity;
                    n.vx = o.vx;
                    n.vy = o.vy;
                    // Insert in order, dropping the farthest when full.
                    if (count == kMoveMaxNeighbours && !moveCloser(n, closest[count - 1])) {
                        continue;
                    }
                    int at = count < kMoveMaxNeighbours ? count++ : count - 1;
                    while (at > 0 && moveCloser(n, closest[at - 1])) {
                        closest[at] = closest[at - 1];
                        --at;
                    }
                    closest[at] = n;
                }
            }
        }

        const bool ghost = mover.ghostSteps > 0;

        // Pawns standing in each other push apart, so a pile (everyone spawned on one cell) spreads.
        if (!ghost) {
            float pushX = 0.0f;
            float pushY = 0.0f;
            for (int k = 0; k < count; ++k) {
                const MoveNeighbour& n = closest[k];
                if (n.dist2 >= reach * reach) {
                    continue;
                }
                const float dist = std::sqrt(n.dist2);
                float awayX = 0.0f;
                float awayY = 0.0f;
                if (dist > 1.0e-4f) {
                    awayX = -n.px / dist;
                    awayY = -n.py / dist;
                } else {
                    // On top of each other: each goes its own way, the same way every time.
                    const uint64_t h = splitMix64(static_cast<uint64_t>(entt::to_integral(a.entity)) * 0x9E3779B1ULL ^
                                                  static_cast<uint64_t>(entt::to_integral(n.entity)));
                    awayX = kMoveDirs[h & 15u][0];
                    awayY = kMoveDirs[h & 15u][1];
                }
                const float overlap = (reach - dist) / reach;
                pushX += awayX * overlap;
                pushY += awayY * overlap;
            }
            wantX += pushX * base * kMoveSeparationSpeed;
            wantY += pushY * base * kMoveSeparationSpeed;
            const float want2 = wantX * wantX + wantY * wantY;
            const float limit = std::max(topSpeed, base * kMoveSeparationSpeed);
            if (want2 > limit * limit) {
                const float scale = limit / std::sqrt(want2);
                wantX *= scale;
                wantY *= scale;
            }
        }
        const float wantLen = std::sqrt(wantX * wantX + wantY * wantY);

        // Candidate velocities: the wanted one turned by up to a half circle, at full and half
        // speed, its two axis parts (to slide along a wall), and standing still. The cheapest one the pawn can reach this step wins.
        // Far from every wall (the farthest a candidate can put it is under 0.8 m away, counting
        // where it would stop), there is nothing to test the candidates against.
        bool roomy = true;
        for (int cy = static_cast<int>(std::floor(a.y - kMoveRoomyReach)); cy <= static_cast<int>(std::floor(a.y + kMoveRoomyReach)) && roomy; ++cy) {
            for (int cx = static_cast<int>(std::floor(a.x - kMoveRoomyReach)); cx <= static_cast<int>(std::floor(a.x + kMoveRoomyReach)); ++cx) {
                if (!map.walkable(CellPos{cx, cy, a.level})) {
                    roomy = false;
                    break;
                }
            }
        }

        float bestCost = 1.0e30f;
        float bestVx = 0.0f;
        float bestVy = 0.0f;
        const int candidates = wantLen > 1.0e-6f ? 17 + 9 + 2 + 1 : 1;
        for (int c = 0; c < candidates; ++c) {
            float ux = 0.0f;
            float uy = 0.0f;
            int turn = 0;
            if (c < 17) {
                turn = c - 8;
            } else if (c < 26) {
                turn = c - 17 - 4;
            }
            if (c == 26 || c == 27) {
                // Slide along a wall: only the part of the wanted velocity along one axis.
                ux = c == 26 ? wantX : 0.0f;
                uy = c == 27 ? wantY : 0.0f;
            } else if (wantLen > 1.0e-6f && c < 26) {
                const float* dir = kMoveDirs[static_cast<unsigned>(turn) & 15u];
                const float scale = c < 17 ? 1.0f : 0.5f;
                ux = (wantX * dir[0] - wantY * dir[1]) * scale;
                uy = (wantX * dir[1] + wantY * dir[0]) * scale;
            }
            // What it can actually do: change its velocity by no more than `accel`.
            float dvx = ux - a.vx;
            float dvy = uy - a.vy;
            const float dv2 = dvx * dvx + dvy * dvy;
            if (dv2 > accel * accel) {
                const float scale = accel / std::sqrt(dv2);
                dvx *= scale;
                dvy *= scale;
            }
            const float nvx = a.vx + dvx;
            const float nvy = a.vy + dvy;

            float cost = ((nvx - wantX) * (nvx - wantX) + (nvy - wantY) * (nvy - wantY)) / (base * base);
            const int steps = turn < 0 ? -turn : turn;
            cost += kMoveTurnPenalty * static_cast<float>(steps) + (turn > 0 ? kMoveLeftExtra : 0.0f);
            // Walls: it must fit where it will be next step and where it would come to rest if
            // it braked from there, so it never builds up speed it can't lose before a wall.
            const float speed = std::sqrt(nvx * nvx + nvy * nvy);
            const float rest = 1.0f + 0.5f * speed / accel;
            if (!roomy && (!pawnFitsAt(map, a.x + nvx, a.y + nvy, kPawnRadius, a.level) ||
                           !pawnFitsAt(map, a.x + nvx * rest, a.y + nvy * rest, kPawnRadius, a.level))) {
                cost += kMoveWallPenalty;
            }
            if (!ghost) {
                for (int k = 0; k < count; ++k) {
                    const MoveNeighbour& n = closest[k];
                    const float t = moveTimeToMeet(n.px, n.py, nvx - n.vx, nvy - n.vy, reach);
                    if (t < kMoveHorizon) {
                        cost += kMoveCollisionWeight * (kMoveHorizon - t) / kMoveHorizon;
                    }
                    // And room to spare: the closer to it after this step, the dearer.
                    const float rx = n.px + n.vx - nvx;
                    const float ry = n.py + n.vy - nvy;
                    const float after = std::sqrt(rx * rx + ry * ry);
                    if (after < reach + kMoveSpace) {
                        cost += kMoveSpaceWeight * (reach + kMoveSpace - after) / kMoveSpace;
                    }
                }
            }
            if (cost < bestCost) {
                bestCost = cost;
                bestVx = nvx;
                bestVy = nvy;
            }
        }
        if (bestVx * bestVx + bestVy * bestVy < 1.0e-10f) {
            bestVx = 0.0f;
            bestVy = 0.0f;
        }
        // A closed door's cell can't be entered: the pawn waits at its edge while the door opens.
        const CellPos into{static_cast<int>(std::floor(a.x + bestVx)), static_cast<int>(std::floor(a.y + bestVy)), a.level};
        if (!(into == here) && map.inBounds(into) && map.passage(into) == Passage::Door) {
            const Entity door = map.buildingAt(into);
            requestDoor(door);
            const Door* state = reg.try_get<Door>(door);
            if (state != nullptr && !state->open) {
                bestVx = 0.0f;
                bestVy = 0.0f;
            }
        }
        a.newVx = bestVx;
        a.newVy = bestVy;

        // Held back for too long: first walk through the other pawns for a while, and if that
        // doesn't help (a wall in the way), give up the walk.
        const bool wantsToWalk = walking && wantLen > 1.0e-6f;
        // How much of the wanted velocity it gets: along the wanted direction, so sliding sideways along a
        // wall it can't go through is not progress (it used to count, and the pawn never gave up).
        const float got = wantLen > 1.0e-6f ? (bestVx * wantX + bestVy * wantY) / wantLen : 0.0f;
        if (mover.ghostSteps > 0) {
            --mover.ghostSteps;
        } else if (wantsToWalk && got < kMoveHeldBackShare * wantLen) {
            ++heldUp_;
            if (mover.blockedSteps < 0xFFFFu) {
                ++mover.blockedSteps;
            }
            if (mover.heldSteps < 0xFFFFu) {
                ++mover.heldSteps;
            }
            if (mover.blockedSteps >= kMoveGhostAfter) {
                mover.ghostSteps = kMoveGhostSteps;
                mover.blockedSteps = 0;
            }
            if (mover.heldSteps >= kMoveGiveUpAfter) {
                mover.path.clear();
                mover.next = 0;
                mover.gaveUp = true;
                mover.heldSteps = 0;
            }
        } else {
            mover.blockedSteps = 0;
            mover.heldSteps = 0;
        }
    }

    // Everyone moves together, from the state they all saw.
    for (const Agent& a : agents_) {
        if (reg.all_of<PlayerControl>(a.entity)) {
            continue;
        }
        Position& pos = reg.get<Position>(a.entity);
        Mover& mover = reg.get<Mover>(a.entity);
        pos.x = a.x + a.newVx;
        pos.y = a.y + a.newVy;
        mover.vx = a.newVx;
        mover.vy = a.newVy;
    }
    for (const Agent& a : agents_) {
        bucketHead_[static_cast<size_t>(a.bucket)] = -1;
    }
}

}  // namespace colony

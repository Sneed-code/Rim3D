#include "ColonySim/Patch.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Resonance.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

PatchSystem::PatchSystem(Things& things, const Map& map, HealthSystem& health, FighterSystem& fighters, const GameTime& time)
    : things_(things), map_(map), health_(health), fighters_(fighters), time_(time) {}

bool PatchSystem::patching(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<Patching>(pawn);
}

bool PatchSystem::safe(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Fighter* f = reg.try_get<Fighter>(pawn);
    if (f == nullptr) {
        return true;  // not in a fight
    }
    const Position* pos = reg.try_get<Position>(pawn);
    const int64_t now = time_.steps;
    for (const KnownEnemy& k : f->known) {
        if (now - k.seenAt > rules_.safeSeenSteps || health_.incapacitated(k.pawn)) {
            continue;
        }
        if (pos == nullptr || std::hypot(k.x - pos->x, k.y - pos->y) <= rules_.safeRange) {
            return false;
        }
    }
    return true;
}

bool PatchSystem::patch(Entity pawn, bool player, std::string& error) {
    entt::registry& reg = things_.registry();
    if (!things_.exists(pawn) || !health_.tracks(pawn)) {
        error = "that pawn has no body";
        return false;
    }
    if (health_.incapacitated(pawn) || reg.any_of<KnockedDown, Stunned, Casting>(pawn)) {
        error = "that pawn can't patch itself now";
        return false;
    }
    if (reg.all_of<Patching>(pawn)) {
        error = "already patching";
        return false;
    }
    const int zone = health_.worstBleedingZone(pawn);
    if (zone < 0) {
        error = "nothing is bleeding";
        return false;
    }
    Patching& p = reg.emplace<Patching>(pawn);
    p.zone = zone;
    p.stepsLeft = health_.patchSteps(health_.zoneBleedLevel(pawn, zone));
    p.player = player;
    // It stands still while it patches: an aim is dropped, a fighter stops, a job waits (JobSystem's pause gate).
    if (Shooter* s = reg.try_get<Shooter>(pawn); s != nullptr && s->phase == ShotPhase::Aiming) {
        s->phase = ShotPhase::Ready;
        s->stepsLeft = 0;
        s->shotsLeft = 0;
    }
    if (Mover* mover = reg.try_get<Mover>(pawn)) {
        mover->vx = 0.0f;
        mover->vy = 0.0f;
    }
    if (reg.all_of<Fighter>(pawn)) {
        fighters_.replan(pawn);
    }
    emitAction(pawn, zone, p.stepsLeft, "patching", 0);
    return true;
}

void PatchSystem::step() {
    entt::registry& reg = things_.registry();
    // Patches under way.
    scratch_.assign(reg.view<Patching>().begin(), reg.view<Patching>().end());
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        Patching& p = reg.get<Patching>(pawn);
        if (health_.incapacitated(pawn) || !things_.spawned(pawn)) {
            reg.remove<Patching>(pawn);  // knocked out mid-patch: the wound stays open
            continue;
        }
        if (--p.stepsLeft > 0) {
            continue;
        }
        const int zone = p.zone;
        reg.remove<Patching>(pawn);
        if (health_.patchZone(pawn, zone, health_.patchQuality()) > 0) {
            emitAction(pawn, zone, 0, "patched", 1);
        }
    }
    // The AI: a person that bleeds patches itself when it is safe. The player's pawn waits for PatchCommand.
    const int64_t now = time_.steps;
    scratch_.clear();
    for (const auto [pawn, health] : reg.view<Health>().each()) {
        if (!health.dead && !health.downed && !health.zoneBleeds.empty() &&
            std::any_of(health.zoneBleeds.begin(), health.zoneBleeds.end(), [](uint8_t b) { return b > 0; })) {
            scratch_.push_back(pawn);
        }
    }
    std::sort(scratch_.begin(), scratch_.end());
    for (const Entity pawn : scratch_) {
        if ((now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.thinkEvery != 0 || !things_.spawned(pawn) ||
            reg.any_of<PlayerControl, Patching, KnockedDown, Stunned, Casting>(pawn) || !safe(pawn)) {
            continue;
        }
        std::string ignored;
        patch(pawn, false, ignored);
    }
    crawl();
}

Entity PatchSystem::crawlThreat(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Position* pos = reg.try_get<Position>(pawn);
    if (pos == nullptr) {
        return kNoEntity;
    }
    const FactionId side = fighters_.factionOf(pawn);
    Entity best = kNoEntity;
    float bestD = rules_.crawlRange;
    for (const auto [other, f, at] : reg.view<const Fighter, const Position>().each()) {
        if (other == pawn || at.level != pos->level || fighters_.factionOf(other) == side || health_.incapacitated(other) ||
            !things_.spawned(other)) {
            continue;
        }
        const float d = std::hypot(at.x - pos->x, at.y - pos->y);
        if (d < bestD || (d == bestD && best != kNoEntity && other < best)) {
            bestD = d;
            best = other;
        }
    }
    return best;
}

// Part D: a person downed by the torso and awake drags itself a cell at a time away from the nearest able enemy within
// crawlRange; with none near it lies still. Not while carried (a rescuer or a hauler has it).
void PatchSystem::crawl() {
    entt::registry& reg = things_.registry();
    const int64_t now = time_.steps;
    scratch_.clear();
    for (const auto [pawn, health] : reg.view<Health>().each()) {
        if (health.crawls && health.downed && !health.dead) {
            scratch_.push_back(pawn);
        }
    }
    if (scratch_.empty()) {
        return;
    }
    std::sort(scratch_.begin(), scratch_.end());
    std::vector<Entity> carried;
    for (const auto [carrier, c] : reg.view<const Carrying>().each()) {
        carried.push_back(c.thing);
    }
    for (const auto [rescuer, f] : reg.view<const Fighter>().each()) {
        if (f.lifted) {
            carried.push_back(f.rescuing);
        }
    }
    for (const Entity pawn : scratch_) {
        if ((now + static_cast<int64_t>(entt::to_integral(pawn))) % rules_.thinkEvery != 0 || !things_.spawned(pawn) ||
            !reg.all_of<Mover, Position>(pawn)) {
            continue;
        }
        Mover& mover = reg.get<Mover>(pawn);
        if (std::find(carried.begin(), carried.end(), pawn) != carried.end()) {
            mover.path.clear();
            mover.next = 0;
            continue;
        }
        if (!mover.arrived()) {
            continue;  // still dragging itself to the last cell
        }
        const Entity threat = crawlThreat(pawn);
        if (threat == kNoEntity) {
            continue;
        }
        const Position& pos = reg.get<Position>(pawn);
        const Position& from = reg.get<Position>(threat);
        const CellPos here = cellOf(pos);
        float bestD = std::hypot(pos.x - from.x, pos.y - from.y);
        CellPos best = here;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const CellPos c{here.x + dx, here.y + dy, here.level};
                if ((dx == 0 && dy == 0) || !map_.walkable(c)) {
                    continue;
                }
                const float cx = static_cast<float>(c.x) + 0.5f;
                const float cy = static_cast<float>(c.y) + 0.5f;
                const float d = std::hypot(cx - from.x, cy - from.y);
                if (d > bestD + 0.1f && straightWalkIsClear(map_, pos.x, pos.y, cx, cy, kPawnRadius, here.level)) {
                    bestD = d;
                    best = c;
                }
            }
        }
        if (best != here) {
            mover.walkTo(pos.x, pos.y, {best});
        }
    }
}

void PatchSystem::emitAction(Entity pawn, int zone, int steps, const char* message, int count) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = EventType::PawnAction;
    e.action = PawnAction::Patch;
    e.thing = pawn;
    e.target = pawn;
    e.part = zone;
    e.amount = static_cast<float>(steps);
    e.message = message;
    e.count = count;
    events_->push_back(e);
}

void PatchSystem::hashThing(Entity pawn, StateHasher& h) const {
    if (const Patching* p = things_.registry().try_get<Patching>(pawn)) {
        h.add(p->zone);
        h.add(p->stepsLeft);
        h.add(p->player);
    }
}

}  // namespace colony

#include "ColonySim/BodyMotion.h"

#include "ColonySim/Combat.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Map.h"
#include "ColonySim/Movement.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

constexpr float kMotionSampleRate = 30.0f;  // path samples per clip second
constexpr float kMotionStepSeconds = 1.0f / 30.0f;  // one sim step of real time at 1x

// Sine and cosine of a turn in degrees, by plain arithmetic (a polynomial), so every compiler gives the same bits:
// the sim must play the same game on Linux and Windows. Good to about 1e-6 over the whole circle.
void motionSinCos(float degrees, float& s, float& c) {
    float d = std::fmod(degrees, 360.0f);
    if (d > 180.0f) {
        d -= 360.0f;
    } else if (d < -180.0f) {
        d += 360.0f;
    }
    // Fold into [-90, 90]: sin(180 - x) = sin x, cos(180 - x) = -cos x.
    float sign = 1.0f;
    if (d > 90.0f) {
        d = 180.0f - d;
        sign = -1.0f;
    } else if (d < -90.0f) {
        d = -180.0f - d;
        sign = -1.0f;
    }
    const float x = d * 0.017453292519943295f;
    const float x2 = x * x;
    s = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
    c = sign * (1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f + x2 * (-1.0f / 3628800.0f))))));
}

float motionEase(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

int motionSideOf(float faceX, float faceY, float pushX, float pushY) {
    const float f = pushX * faceX + pushY * faceY;     // along the way it faces
    const float r = pushY * faceX - pushX * faceY;     // to its right
    if (std::fabs(f) >= std::fabs(r)) {
        return f < 0.0f ? 0 : 1;
    }
    return r < 0.0f ? 2 : 3;
}

void MotionClip::sample(float t, float& f, float& r, float& turn) const {
    f = r = turn = 0.0f;
    if (forward.empty()) {
        return;
    }
    const float pos = std::max(0.0f, t) * kMotionSampleRate;
    const size_t last = forward.size() - 1;
    const size_t i = std::min(static_cast<size_t>(pos), last);
    if (i >= last) {
        f = forward[last];
        r = right[last];
        turn = yaw[last];
        return;
    }
    const float a = pos - static_cast<float>(i);
    f = forward[i] + (forward[i + 1] - forward[i]) * a;
    r = right[i] + (right[i + 1] - right[i]) * a;
    turn = yaw[i] + (yaw[i + 1] - yaw[i]) * a;
}

BodyMotionSystem::BodyMotionSystem(Things& things, Map& map) : things_(things), map_(map) {
    const DefDatabase& defs = things_.defs();
    if (const Def* r = defs.find("Rules", "MotionRules")) {
        rules_.stepMetres = std::clamp(static_cast<float>(r->number("stepMetres", rules_.stepMetres)), 0.01f, 0.25f);
        rules_.bodyRadius = std::max(0.05f, static_cast<float>(r->number("bodyRadius", rules_.bodyRadius)));
        rules_.minBodyRadius = std::max(0.05f, static_cast<float>(r->number("minBodyRadius", rules_.minBodyRadius)));
        rules_.maxBodyRadius = std::max(rules_.minBodyRadius, static_cast<float>(r->number("maxBodyRadius", rules_.maxBodyRadius)));
        rules_.heightFollow = std::clamp(static_cast<float>(r->number("heightFollow", rules_.heightFollow)), 0.0f, 1.0f);
        rules_.animalScalePerRoot = std::max(0.0f, static_cast<float>(r->number("animalScalePerRoot", rules_.animalScalePerRoot)));
        rules_.rollClip = r->text("rollClip", rules_.rollClip);
        const char* sides[4] = {"back", "forward", "left", "right"};
        for (int i = 0; i < 2; ++i) {
            rules_.sidestepClip[i] = r->text((std::string("sidestepClip.") + sides[2 + i]).c_str(), rules_.sidestepClip[i]);
        }
        for (int i = 0; i < 4; ++i) {
            rules_.staggerClip[i] = r->text((std::string("staggerClip.") + sides[i]).c_str(), rules_.staggerClip[i]);
            rules_.flinchClip[i] = r->text((std::string("flinchClip.") + sides[i]).c_str(), rules_.flinchClip[i]);
            rules_.knockdownClip[i] = r->text((std::string("knockdownClip.") + sides[i]).c_str(), rules_.knockdownClip[i]);
        }
        rules_.getUpClip = r->text("getUpClip", rules_.getUpClip);
        rules_.staggerMetres = std::max(0.0f, static_cast<float>(r->number("staggerMetres", rules_.staggerMetres)));
        rules_.staggerSteps = std::max(1, static_cast<int>(r->integer("staggerSteps", rules_.staggerSteps)));
        rules_.knockdownMetres = std::max(0.0f, static_cast<float>(r->number("knockdownMetres", rules_.knockdownMetres)));
        rules_.knockdownSteps = std::max(1, static_cast<int>(r->integer("knockdownSteps", rules_.knockdownSteps)));
        rules_.sidestepMetres = std::max(0.0f, static_cast<float>(r->number("sidestepMetres", rules_.sidestepMetres)));
        rules_.sidestepSteps = std::max(1, static_cast<int>(r->integer("sidestepSteps", rules_.sidestepSteps)));
        rules_.blastSteps = std::max(1, static_cast<int>(r->integer("blastSteps", rules_.blastSteps)));
        rules_.trunkBodyRadiusMax = std::max(0.05f, static_cast<float>(r->number("trunkBodyRadiusMax", rules_.trunkBodyRadiusMax)));
        rules_.propRadius = std::max(0.0f, static_cast<float>(r->number("propRadius", rules_.propRadius)));
        rules_.bodyKgPerSize = std::max(1.0f, static_cast<float>(r->number("bodyKgPerSize", rules_.bodyKgPerSize)));
        rules_.propMaxMassShare = std::max(0.0f, static_cast<float>(r->number("propMaxMassShare", rules_.propMaxMassShare)));
        rules_.propCellMargin = std::clamp(static_cast<float>(r->number("propCellMargin", rules_.propCellMargin)), 0.0f, 0.5f);
    }
    // Loose props: items whose def (or a parent) says `minor`.
    propKg_.assign(defs.size(), -1.0f);
    for (const DefId id : defs.idsOfKind("Things")) {
        const Def& def = defs.get(id);
        if (def.text("category") == "Item" && def.flag("minor", false)) {
            propKg_[id.value] = std::max(0.0f, static_cast<float>(def.number("stats.mass", 1.0)));
        }
    }
    byDef_.assign(defs.size(), -1);
    for (const DefId id : defs.idsOfKind("MotionClips")) {
        const Def& def = defs.get(id);
        MotionClip c;
        c.def = id;
        c.name = def.defName();
        c.kind = def.text("kind");
        c.anim = def.text("anim");
        c.from = std::max(0.0f, static_cast<float>(def.number("from", 0.0)));
        c.hitSeconds = static_cast<float>(def.number("hitSeconds", -1.0));
        c.keepWhenDown = def.flag("keepWhenDown", false);
        const size_t n = def.count("root");
        for (size_t i = 0; i < n; ++i) {
            const std::string base = "root." + std::to_string(i);
            c.forward.push_back(static_cast<float>(def.numberAt(base.c_str(), 0, 0.0)));
            c.right.push_back(static_cast<float>(def.numberAt(base.c_str(), 1, 0.0)));
            c.yaw.push_back(static_cast<float>(def.numberAt(base.c_str(), 2, 0.0)));
        }
        if (c.forward.empty()) {
            c.forward.push_back(0.0f);
            c.right.push_back(0.0f);
            c.yaw.push_back(0.0f);
        }
        const float measured = static_cast<float>(c.forward.size() - 1) / kMotionSampleRate;
        c.seconds = std::max(kMotionStepSeconds, static_cast<float>(def.number("seconds", measured)));
        byDef_[id.value] = static_cast<int32_t>(clips_.size());
        byName_[c.name] = clips_.size();
        clips_.push_back(std::move(c));
    }
}

BodyMotionSystem::~BodyMotionSystem() = default;

const MotionClip* BodyMotionSystem::clip(DefId def) const {
    if (!def.valid() || def.value >= byDef_.size() || byDef_[def.value] < 0) {
        return nullptr;
    }
    return &clips_[static_cast<size_t>(byDef_[def.value])];
}

const MotionClip* BodyMotionSystem::clip(const std::string& name) const {
    const auto it = byName_.find(name);
    return it != byName_.end() ? &clips_[it->second] : nullptr;
}

DefId BodyMotionSystem::clipId(const std::string& name) const {
    const MotionClip* c = clip(name);
    return c != nullptr ? c->def : DefId{};
}

float BodyMotionSystem::mass(Entity pawn) const {
    return mass_ ? std::max(0.05f, mass_(pawn)) : 1.0f;
}

float BodyMotionSystem::sizeScale(Entity pawn) const {
    const float h = height_ ? height_(pawn) : 0.0f;
    if (h > 0.0f) {
        return 1.0f + (std::clamp(h, 0.5f, 1.5f) - 1.0f) * rules_.heightFollow;
    }
    return 1.0f;
}

float BodyMotionSystem::bodyRadius(Entity pawn) const {
    return std::clamp(rules_.bodyRadius * std::sqrt(mass(pawn)), rules_.minBodyRadius, rules_.maxBodyRadius);
}

bool BodyMotionSystem::play(Entity pawn, DefId clipDef, float faceX, float faceY, int steps, bool turnBody) {
    const MotionClip* c = clip(clipDef);
    if (c == nullptr) {
        return false;
    }
    const int length = steps > 0 ? steps : std::max(1, static_cast<int>(std::lround(c->seconds / kMotionStepSeconds)));
    return playTimed(pawn, clipDef, c->seconds, length, faceX, faceY, turnBody);
}

bool BodyMotionSystem::playTimed(Entity pawn, DefId clipDef, float atSeconds, int steps, float faceX, float faceY, bool turnBody) {
    entt::registry& reg = things_.registry();
    const MotionClip* c = clip(clipDef);
    if (c == nullptr || !reg.valid(pawn) || !reg.all_of<Position, Posture>(pawn)) {
        return false;
    }
    const Posture& posture = reg.get<Posture>(pawn);
    float len = std::sqrt(faceX * faceX + faceY * faceY);
    if (!(len > 1e-4f)) {
        faceX = posture.facingX;
        faceY = posture.facingY;
        len = std::sqrt(faceX * faceX + faceY * faceY);
    }
    BodyMotion& m = reg.get_or_emplace<BodyMotion>(pawn);
    m = BodyMotion{};
    m.clip = clipDef;
    m.faceX = len > 1e-6f ? faceX / len : 1.0f;
    m.faceY = len > 1e-6f ? faceY / len : 0.0f;
    m.scale = sizeScale(pawn);
    m.turnBody = turnBody;
    const float at = std::clamp(atSeconds, kMotionStepSeconds, c->seconds);
    m.rate = std::clamp(at / (static_cast<float>(std::max(1, steps)) * kMotionStepSeconds), 0.05f, 20.0f);
    return true;
}

bool BodyMotionSystem::push(Entity pawn, float dx, float dy, float metres, int steps, int from, int to) {
    entt::registry& reg = things_.registry();
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!reg.valid(pawn) || !reg.all_of<Position>(pawn) || !(len > 1e-4f) || steps <= 0) {
        return false;
    }
    BodyMotion& m = reg.get_or_emplace<BodyMotion>(pawn);
    m = BodyMotion{};
    m.faceX = dx / len;
    m.faceY = dy / len;
    m.pushMetres = std::max(0.0f, metres);
    m.pushSteps = steps;
    m.pushFrom = std::clamp(from, 0, steps);
    m.pushTo = to < 0 ? steps : std::clamp(to, m.pushFrom, steps);
    m.turnBody = false;
    return true;
}

bool BodyMotionSystem::throwBody(Entity pawn, float pushX, float pushY, bool knockdown, float strength, bool plainPush) {
    entt::registry& reg = things_.registry();
    const float len = std::sqrt(pushX * pushX + pushY * pushY);
    if (!reg.valid(pawn) || !reg.all_of<Position, Posture>(pawn) || !(len > 1e-4f)) {
        return false;
    }
    const Posture& posture = reg.get<Posture>(pawn);
    if (hasClips(pawn)) {
        const int side = motionSideOf(posture.facingX, posture.facingY, pushX, pushY);
        const DefId c = clipId(knockdown ? rules_.knockdownClip[side] : plainPush ? rules_.staggerClip[side] : rules_.flinchClip[side]);
        if (c.valid() && play(pawn, c, posture.facingX, posture.facingY, 0, false)) {
            // A stagger or a flinch is the upper body's: the legs walk the stumble the clip carries the body through
            // (sneed 2026-11-18, third time asked). A knockdown is the whole body's fall.
            setLayers(pawn, !knockdown, false);
            return true;
        }
    }
    if (!plainPush) {
        return false;
    }
    const float size = std::sqrt(mass(pawn));
    const float scale = hasClips(pawn) ? sizeScale(pawn) : (size > 0.0f ? rules_.animalScalePerRoot / size : 1.0f);
    const float metres = (knockdown ? rules_.knockdownMetres : rules_.staggerMetres) * std::max(0.0f, strength) * scale;
    return push(pawn, pushX, pushY, metres, knockdown ? rules_.knockdownSteps : rules_.staggerSteps);
}

bool BodyMotionSystem::sidestep(Entity pawn, float dx, float dy) {
    entt::registry& reg = things_.registry();
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!reg.valid(pawn) || !reg.all_of<Position, Posture>(pawn) || !(len > 1e-4f)) {
        return false;
    }
    const Posture& posture = reg.get<Posture>(pawn);
    const int side = motionSideOf(posture.facingX, posture.facingY, dx, dy);
    if (side >= 2 && hasClips(pawn)) {
        const DefId c = clipId(rules_.sidestepClip[side - 2]);
        if (c.valid() && play(pawn, c, posture.facingX, posture.facingY, 0, false)) {
            return true;
        }
    }
    return push(pawn, dx, dy, rules_.sidestepMetres * sizeScale(pawn), rules_.sidestepSteps);
}

void BodyMotionSystem::setLayers(Entity pawn, bool upper, bool freeFeet) {
    entt::registry& reg = things_.registry();
    if (BodyMotion* m = reg.valid(pawn) ? reg.try_get<BodyMotion>(pawn) : nullptr) {
        m->upper = upper;
        m->freeFeet = freeFeet;
    }
}

bool BodyMotionSystem::holdsBody(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const BodyMotion* m = reg.valid(pawn) ? reg.try_get<BodyMotion>(pawn) : nullptr;
    return m != nullptr && !m->freeFeet;
}

bool BodyMotionSystem::bodiesClear(Entity pawn, float fromX, float fromY, float x, float y, int level) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || (solid_ && !solid_(pawn))) {
        return true;
    }
    const float mine = bodyRadius(pawn);
    for (const auto [e, pos, posture] : reg.view<const Position, const Posture>().each()) {
        (void)posture;
        if (e == pawn || pos.level != level || !things_.spawned(e) || (solid_ && !solid_(e))) {
            continue;
        }
        const float r = mine + bodyRadius(e);
        const float dx = x - pos.x;
        const float dy = y - pos.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= r * r) {
            continue;
        }
        // Overlapping there: allowed only when it moves away from that body (out of an overlap it already had).
        const float fx = fromX - pos.x;
        const float fy = fromY - pos.y;
        if (d2 <= fx * fx + fy * fy) {
            return false;
        }
    }
    return true;
}

void BodyMotionSystem::stop(Entity pawn) {
    entt::registry& reg = things_.registry();
    if (reg.valid(pawn)) {
        reg.remove<BodyMotion>(pawn);
    }
}

bool BodyMotionSystem::moving(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(pawn) && reg.all_of<BodyMotion>(pawn);
}

float BodyMotionSystem::forwardAt(DefId clipDef, float t) const {
    const MotionClip* c = clip(clipDef);
    if (c == nullptr) {
        return 0.0f;
    }
    float f = 0.0f;
    float r = 0.0f;
    float y = 0.0f;
    c->sample(t, f, r, y);
    return f;
}

bool BodyMotionSystem::fits(Entity pawn, float fromX, float fromY, float x, float y, int level) const {
    // Between walls a body is as wide as the pathfinder thinks (kPawnRadius), so a body never ends up where no path
    // can get it out. Tree trunks stop the body itself (its own radius); one already against a trunk may leave it.
    if (!pawnFitsAt(map_, x, y, kPawnRadius, level)) {
        return false;
    }
    return plants_ == nullptr || trunksAllow(*plants_, fromX, fromY, x, y, trunkRadius(pawn), level);
}

float BodyMotionSystem::trunkRadius(Entity pawn) const {
    return std::min(bodyRadius(pawn), rules_.trunkBodyRadiusMax);
}

float BodyMotionSystem::propMass(Entity item) const {
    const entt::registry& reg = things_.registry();
    const Thing* thing = reg.try_get<Thing>(item);
    if (thing == nullptr || thing->def.value >= propKg_.size() || propKg_[thing->def.value] < 0.0f) {
        return -1.0f;
    }
    const StackCount* stack = reg.try_get<StackCount>(item);
    return propKg_[thing->def.value] * static_cast<float>(stack != nullptr ? std::max(1, stack->count) : 1);
}

void BodyMotionSystem::shoveProps() {
    if (rules_.propRadius <= 0.0f) {
        return;
    }
    entt::registry& reg = things_.registry();
    order_.clear();
    for (const auto [e, pos, posture] : reg.view<const Position, const Posture>().each()) {
        (void)pos;
        (void)posture;
        if (things_.spawned(e) && (!solid_ || solid_(e))) {
            order_.push_back(e);
        }
    }
    std::sort(order_.begin(), order_.end());
    for (const Entity pawn : order_) {
        const Position& at = reg.get<Position>(pawn);
        const float bodyKg = mass(pawn) * rules_.bodyKgPerSize;
        const float minD = bodyRadius(pawn) + rules_.propRadius;
        const int minX = static_cast<int>(std::floor(at.x - minD));
        const int maxX = static_cast<int>(std::floor(at.x + minD));
        const int minY = static_cast<int>(std::floor(at.y - minD));
        const int maxY = static_cast<int>(std::floor(at.y + minD));
        for (int cy = minY; cy <= maxY; ++cy) {
            for (int cx = minX; cx <= maxX; ++cx) {
                const CellPos cell{cx, cy, at.level};
                if (!map_.inBounds(cell)) {
                    continue;
                }
                for (const Entity item : things_.itemsAt(cell)) {
                    const float kg = propMass(item);
                    if (kg < 0.0f || kg > bodyKg * rules_.propMaxMassShare) {
                        continue;
                    }
                    Position& ip = reg.get<Position>(item);
                    float ox = ip.x - at.x;
                    float oy = ip.y - at.y;
                    const float d2 = ox * ox + oy * oy;
                    if (d2 >= minD * minD) {
                        continue;
                    }
                    float d = std::sqrt(d2);
                    if (d < 1e-4f) {
                        // Right under the body: out the way the body faces, else along +x.
                        const Posture* posture = reg.try_get<Posture>(pawn);
                        const float fl = posture != nullptr ? std::sqrt(posture->facingX * posture->facingX + posture->facingY * posture->facingY) : 0.0f;
                        ox = fl > 1e-6f ? posture->facingX / fl : 1.0f;
                        oy = fl > 1e-6f ? posture->facingY / fl : 0.0f;
                        d = 0.0f;
                    } else {
                        ox /= d;
                        oy /= d;
                    }
                    const float shove = (minD - d) * bodyKg / (bodyKg + kg);
                    const float lo = rules_.propCellMargin;
                    const float nx = std::clamp(ip.x + ox * shove, static_cast<float>(cx) + lo, static_cast<float>(cx + 1) - lo);
                    const float ny = std::clamp(ip.y + oy * shove, static_cast<float>(cy) + lo, static_cast<float>(cy + 1) - lo);
                    if (nx != ip.x || ny != ip.y) {
                        ip.x = nx;
                        ip.y = ny;
                        ++propShoves_;
                    }
                }
            }
        }
    }
}

void BodyMotionSystem::gatherBodies(Entity pawn, float x, float y, int level, float range) {
    near_.clear();
    const entt::registry& reg = things_.registry();
    for (const auto [e, pos, posture] : reg.view<const Position, const Posture>().each()) {
        (void)posture;
        if (e == pawn || pos.level != level || !things_.spawned(e) || (solid_ && !solid_(e))) {
            continue;
        }
        const float dx = pos.x - x;
        const float dy = pos.y - y;
        const float r = range + bodyRadius(e);
        if (dx * dx + dy * dy <= r * r) {
            near_.push_back(e);
        }
    }
    std::sort(near_.begin(), near_.end());
}

float BodyMotionSystem::sweep(Entity pawn, float dx, float dy) {
    entt::registry& reg = things_.registry();
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!reg.valid(pawn) || !reg.all_of<Position>(pawn) || !(len > 1e-6f)) {
        return 0.0f;
    }
    ++sweeps_;
    Position& pos = reg.get<Position>(pawn);
    const float myRadius = bodyRadius(pawn);
    const float myMass = mass(pawn);
    gatherBodies(pawn, pos.x, pos.y, pos.level, len + myRadius + 0.05f);
    const int pieces = std::max(1, static_cast<int>(std::ceil(len / rules_.stepMetres)));
    const float sx = dx / static_cast<float>(pieces);
    const float sy = dy / static_cast<float>(pieces);
    float moved = 0.0f;
    bool blocked = false;
    for (int i = 0; i < pieces; ++i) {
        // The ground first: the whole piece, or sliding along a wall on one axis, or stopping.
        float px = 0.0f;
        float py = 0.0f;
        const float tries[3][2] = {{sx, sy}, {sx, 0.0f}, {0.0f, sy}};
        bool found = false;
        for (const auto& t : tries) {
            if ((t[0] != 0.0f || t[1] != 0.0f) && fits(pawn, pos.x, pos.y, pos.x + t[0], pos.y + t[1], pos.level)) {
                px = t[0];
                py = t[1];
                found = true;
                break;
            }
        }
        if (!found) {
            blocked = true;
            break;
        }
        float nx = pos.x + px;
        float ny = pos.y + py;
        // Then bodies: each one the body now overlaps is shoved out by the share of the overlap its weight allows
        // (as far as its own ground lets it), and this body is pushed back by the rest.
        for (const Entity other : near_) {
            Position& op = reg.get<Position>(other);
            const float minD = myRadius + bodyRadius(other);
            float ox = op.x - nx;
            float oy = op.y - ny;
            const float d2 = ox * ox + oy * oy;
            if (d2 >= minD * minD) {
                continue;
            }
            float d = std::sqrt(d2);
            if (d < 1e-4f) {
                // On the same spot: out the way this body is going.
                const float pl = std::sqrt(px * px + py * py);
                ox = pl > 1e-6f ? px / pl : 1.0f;
                oy = pl > 1e-6f ? py / pl : 0.0f;
                d = 0.0f;
            } else {
                ox /= d;
                oy /= d;
            }
            const float overlap = minD - d;
            const float theirMass = mass(other);
            const float share = myMass / (myMass + theirMass);
            float shove = overlap * share;
            if (shove > 0.0f) {
                if (fits(other, op.x, op.y, op.x + ox * shove, op.y + oy * shove, op.level)) {
                    op.x += ox * shove;
                    op.y += oy * shove;
                    ++shoves_;
                } else {
                    shove = 0.0f;  // it's against a wall: it doesn't give
                }
            }
            const float back = overlap - shove;
            if (back > 0.0f) {
                const float bx = nx - ox * back;
                const float by = ny - oy * back;
                if (fits(pawn, pos.x, pos.y, bx, by, pos.level)) {
                    nx = bx;
                    ny = by;
                } else {
                    nx = pos.x;
                    ny = pos.y;
                }
                blocked = true;
            }
        }
        const float mx = nx - pos.x;
        const float my = ny - pos.y;
        moved += std::sqrt(mx * mx + my * my);
        pos.x = nx;
        pos.y = ny;
    }
    if (blocked) {
        ++stopped_;
    }
    return moved;
}

void BodyMotionSystem::step() {
    entt::registry& reg = things_.registry();
    order_.clear();
    for (const Entity e : reg.view<BodyMotion>()) {
        order_.push_back(e);
    }
    if (order_.empty()) {
        return;
    }
    std::sort(order_.begin(), order_.end());
    for (const Entity pawn : order_) {
        BodyMotion& m = reg.get<BodyMotion>(pawn);
        const MotionClip* c = clip(m.clip);
        const bool keep = c != nullptr && c->keepWhenDown;
        if (!things_.spawned(pawn) || !reg.all_of<Position>(pawn) || (able_ && !keep && !able_(pawn))) {
            reg.remove<BodyMotion>(pawn);  // it ends where the body is
            continue;
        }
        float wantF = 0.0f;
        float wantR = 0.0f;
        float wantYaw = 0.0f;
        bool done = false;
        if (c != nullptr) {
            m.time = std::min(c->seconds, m.time + m.rate * kMotionStepSeconds);
            c->sample(m.time, wantF, wantR, wantYaw);
            done = m.time >= c->seconds - 1e-5f;
        } else {
            m.time += 1.0f;
            const int now = static_cast<int>(m.time);
            const int span = std::max(1, m.pushTo - m.pushFrom);
            wantF = m.pushMetres * motionEase(static_cast<float>(now - m.pushFrom) / static_cast<float>(span));
            done = now >= m.pushSteps;
        }
        if (m.freeFeet) {
            // The player's own feet: the clip plays on the upper body and the walking keys move the body.
            m.doneF = wantF;
            m.doneR = wantR;
            m.doneYaw = wantYaw;
            if (done) {
                reg.remove<BodyMotion>(pawn);
            }
            continue;
        }
        const float df = wantF - m.doneF;
        const float dr = wantR - m.doneR;
        m.doneF = wantF;
        m.doneR = wantR;
        // Forward and right in the frame the body faced when it began. Right is the pawn's own right as the game
        // draws it: Unreal looks down on the map with +y to the right of +x, so right of (x, y) is (-y, x).
        const float scale = m.scale;
        const float wx = (df * m.faceX - dr * m.faceY) * scale;
        const float wy = (df * m.faceY + dr * m.faceX) * scale;
        const float want = std::sqrt(wx * wx + wy * wy);
        if (want > 1e-6f) {
            const float got = sweep(pawn, wx, wy);
            m.lost += std::max(0.0f, want - got);
        }
        if (m.turnBody && c != nullptr && wantYaw != m.doneYaw) {
            if (Posture* posture = reg.try_get<Posture>(pawn)) {
                float s = 0.0f;
                float co = 1.0f;
                motionSinCos(wantYaw, s, co);
                // Turned right by wantYaw from the start: cos * forward + sin * right.
                posture->facingX = co * m.faceX - s * m.faceY;
                posture->facingY = co * m.faceY + s * m.faceX;
            }
        }
        m.doneYaw = wantYaw;
        if (Mover* mover = reg.try_get<Mover>(pawn)) {
            mover->vx = wx;
            mover->vy = wy;
        }
        if (done) {
            if (Mover* mover = reg.try_get<Mover>(pawn)) {
                mover->vx = 0.0f;  // and stands: no speed left over to drift on
                mover->vy = 0.0f;
            }
            reg.remove<BodyMotion>(pawn);  // the body stays where the clip took it
        }
    }
}

void BodyMotionSystem::hashThing(Entity e, StateHasher& h) const {
    const BodyMotion* m = things_.registry().try_get<BodyMotion>(e);
    if (m == nullptr) {
        return;
    }
    h.add(uint64_t{0xB0D7307});
    h.add(m->clip.value);
    h.add(m->time);
    h.add(m->rate);
    h.add(m->faceX);
    h.add(m->faceY);
    h.add(m->scale);
    h.add(m->doneF);
    h.add(m->doneR);
    h.add(m->doneYaw);
    h.add(m->pushMetres);
    h.add(m->pushSteps);
    h.add(m->pushFrom);
    h.add(m->pushTo);
    h.add(m->turnBody);
    h.add(m->lost);
    h.add(m->upper);
    h.add(m->freeFeet);
}

}  // namespace colony

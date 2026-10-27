#include "ColonySim/Lights.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"
#include "ColonySim/Weather.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// A curve from two arrays of a def, `<field>.at` and `<field>.value`.
LightNumbers::Curve lightsReadCurve(const Def& def, const char* field, LightNumbers::Curve fallback) {
    const std::string at = std::string(field) + ".at";
    const std::string value = std::string(field) + ".value";
    const size_t n = std::min(def.count(at.c_str()), def.count(value.c_str()));
    if (n == 0) {
        return fallback;
    }
    LightNumbers::Curve curve;
    for (size_t i = 0; i < n; ++i) {
        curve.emplace_back(static_cast<float>(def.numberAt(at.c_str(), i)), static_cast<float>(def.numberAt(value.c_str(), i)));
    }
    return curve;
}

}  // namespace

float LightNumbers::eval(const Curve& curve, float x) {
    if (curve.empty()) {
        return 1.0f;
    }
    if (x <= curve.front().first) {
        return curve.front().second;
    }
    for (size_t i = 1; i < curve.size(); ++i) {
        if (x <= curve[i].first) {
            const float span = curve[i].first - curve[i - 1].first;
            const float t = span > 0.0f ? (x - curve[i - 1].first) / span : 1.0f;
            return curve[i - 1].second + t * (curve[i].second - curve[i - 1].second);
        }
    }
    return curve.back().second;
}

LightNumbers LightNumbers::read(const DefDatabase& defs) {
    LightNumbers n;
    // Without a LightRules def there is no night and no slowing: the sim behaves as before E3.
    n.skyGlowByHour = {{0.0f, 1.0f}};
    n.workSpeedByGlow = {{0.0f, 1.0f}};
    n.moveSpeedByGlow = {{0.0f, 1.0f}};
    const Def* def = defs.find("Rules", "LightRules");
    if (def == nullptr) {
        return n;
    }
    n.skyGlowByHour = lightsReadCurve(*def, "skyGlowByHour", n.skyGlowByHour);
    n.workSpeedByGlow = lightsReadCurve(*def, "workSpeedByGlow", n.workSpeedByGlow);
    n.moveSpeedByGlow = lightsReadCurve(*def, "moveSpeedByGlow", n.moveSpeedByGlow);
    n.blockingHeightDm = static_cast<float>(def->number("blockingHeightDm", n.blockingHeightDm));
    return n;
}

Lights::Lights(Map& map, Things& things, const Rooms& rooms, const DefDatabase& defs, const GameTime& time)
    : map_(map),
      things_(things),
      rooms_(rooms),
      time_(time),
      numbers_(LightNumbers::read(defs)),
      lamp_(static_cast<size_t>(map.width()) * static_cast<size_t>(map.height()), 0.0f) {
    lightDef_.assign(defs.size(), 0);
    for (uint32_t i = 0; i < defs.size(); ++i) {
        const Def& d = defs.get(DefId{i});
        if (d.kind() == "Things" && d.text("category") == "Building" && d.number("glower.radius") > 0.0) {
            lightDef_[i] = 1;
        }
    }
}

void Lights::update() {
    const uint64_t buildings = map_.buildingChanges();
    const uint64_t roofs = rooms_.roofChanges();
    const uint64_t regions = map_.regions().changeCount();
    if (built_ && !dirty_ && buildings == buildingChanges_ && roofs == roofChanges_ && regions == regionChanges_) {
        return;
    }
    built_ = true;
    dirty_ = false;
    buildingChanges_ = buildings;
    roofChanges_ = roofs;
    regionChanges_ = regions;
    rebuild();
}

// True if something tall stands in a cell strictly between the two (a straight line of cells).
bool Lights::blocked(int x0, int y0, int x1, int y1) const {
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    int x = x0;
    int y = y0;
    while (true) {
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
        if (x == x1 && y == y1) {
            return false;
        }
        const CellPos c{x, y, 0};
        if (map_.passage(c) != Passage::Door &&
            static_cast<float>(map_.obstacleDm(c)) >= numbers_.blockingHeightDm) {
            return true;
        }
    }
}

void Lights::rebuild() {
    ++rebuilds_;
    for (const int i : touched_) {
        lamp_[static_cast<size_t>(i)] = 0.0f;
    }
    touched_.clear();
    sources_.clear();

    const entt::registry& reg = things_.registry();
    for (const auto [building, thing, position] : reg.view<const Thing, const Position>().each()) {
        if (thing.category != ThingCategory::Building || lightDef_[thing.def.value] == 0 ||
            reg.all_of<ConstructionSite>(building) || (active_ && !active_(building))) {
            continue;
        }
        const Def& def = things_.defs().get(thing.def);
        LightSource source;
        source.building = building;
        source.cell = cellOf(position);
        source.radius = static_cast<float>(def.number("glower.radius"));
        source.glow = static_cast<float>(def.number("glower.glow", 1.0));
        sources_.push_back(source);
    }
    std::sort(sources_.begin(), sources_.end(),
              [](const LightSource& a, const LightSource& b) { return a.building < b.building; });

    for (const LightSource& s : sources_) {
        const int reach = static_cast<int>(std::ceil(s.radius));
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const CellPos c{s.cell.x + dx, s.cell.y + dy, 0};
                if (!map_.onGround(c)) {
                    continue;
                }
                const float dist = std::sqrt(static_cast<float>(dx * dx + dy * dy));
                if (dist >= s.radius) {
                    continue;
                }
                const float glow = s.glow * (1.0f - dist / s.radius);
                float& slot = lamp_[static_cast<size_t>(map_.index(c))];
                if (glow <= slot || blocked(s.cell.x, s.cell.y, c.x, c.y)) {
                    continue;
                }
                if (slot == 0.0f) {
                    touched_.push_back(map_.index(c));
                }
                slot = glow;
            }
        }
    }
}

float Lights::skyGlow() const {
    const float hour = static_cast<float>(time_.seconds % kSecondsPerDay) / static_cast<float>(kSecondsPerHour);
    return LightNumbers::eval(numbers_.skyGlowByHour, hour) * (weather_ != nullptr ? weather_->skyGlow() : 1.0f);
}

float Lights::clearSkyGlowAtHour(float hour) const { return LightNumbers::eval(numbers_.skyGlowByHour, hour); }

bool Lights::underOpenSky(CellPos c) const {
    return map_.onGround(c) && map_.roof(c) == Roof::None;
}

float Lights::lampLightAt(CellPos c) const {
    return map_.onGround(c) ? lamp_[static_cast<size_t>(map_.index(c))] : 0.0f;
}

float Lights::lightAt(CellPos c) const {
    if (!map_.onGround(c)) {
        return 0.0f;
    }
    const float lamp = lamp_[static_cast<size_t>(map_.index(c))];
    return map_.roof(c) == Roof::None ? std::max(lamp, std::max(skyGlow(), skyFloor_ ? skyFloor_() : 0.0f)) : lamp;
}

float Lights::workSpeedAt(CellPos c) const {
    return LightNumbers::eval(numbers_.workSpeedByGlow, lightAt(c));
}

float Lights::moveSpeedAt(CellPos c) const {
    return LightNumbers::eval(numbers_.moveSpeedByGlow, lightAt(c));
}

}  // namespace colony

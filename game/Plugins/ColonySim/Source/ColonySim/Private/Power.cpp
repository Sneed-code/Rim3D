// Power nets, generators, consumers and batteries (task E5, docs/design/power.md).
#include "ColonySim/Power.h"

#include "ColonySim/Map.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.
constexpr double kPowerDaysPerStep = 1.0 / static_cast<double>(kSecondsPerDay);

uint64_t powerCellKey(CellPos c) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(c.level)) << 48) |
           (static_cast<uint64_t>(static_cast<uint32_t>(c.y) & 0xFFFFFFu) << 24) |
           static_cast<uint64_t>(static_cast<uint32_t>(c.x) & 0xFFFFFFu);
}

uint32_t powerFind(std::vector<uint32_t>& parent, uint32_t i) {
    while (parent[i] != i) {
        parent[i] = parent[parent[i]];
        i = parent[i];
    }
    return i;
}

void powerUnion(std::vector<uint32_t>& parent, uint32_t a, uint32_t b) {
    a = powerFind(parent, a);
    b = powerFind(parent, b);
    if (a != b) {
        // The lower index stays the root, so a net's id does not depend on the order of unions.
        parent[std::max(a, b)] = std::min(a, b);
    }
}

double powerSmoothStep(double edge0, double edge1, double x) {
    if (x <= edge0) {
        return 0.0;
    }
    if (x >= edge1) {
        return 1.0;
    }
    const double t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0 - 2.0 * t);
}

}  // namespace

PowerNumbers PowerNumbers::read(const DefDatabase& defs) {
    PowerNumbers n;
    const Def* def = defs.find("Rules", "PowerRules");
    if (def == nullptr) {
        return n;
    }
    n.batteryEfficiency = std::clamp(def->number("batteryEfficiency", n.batteryEfficiency), 0.01, 1.0);
    n.restartReserveSeconds = std::max(0.0, def->number("restartReserveSeconds", n.restartReserveSeconds));
    n.sunrise = def->number("sun.sunrise", n.sunrise);
    n.fullFrom = def->number("sun.fullFrom", n.fullFrom);
    n.fullTo = def->number("sun.fullTo", n.fullTo);
    n.sunset = def->number("sun.sunset", n.sunset);
    n.windDefault = std::max(0.0, def->number("windDefault", n.windDefault));
    return n;
}

PowerGrid::PowerGrid(Map& map, Things& things, Rooms& rooms, const DefDatabase& defs, std::vector<Event>& events)
    : map_(map), things_(things), rooms_(rooms), defs_(defs), events_(events), numbers_(PowerNumbers::read(defs)) {}

const PowerInfo& PowerGrid::info(DefId id) const {
    static const PowerInfo none;
    if (!id.valid() || id.value >= defs_.size()) {
        return none;
    }
    if (infos_.size() < defs_.size()) {
        infos_.resize(defs_.size());
        infoKnown_.resize(defs_.size(), 0);
    }
    if (infoKnown_[id.value] == 0) {
        infoKnown_[id.value] = 1;
        const Def& def = defs_.get(id);
        PowerInfo& p = infos_[id.value];
        p.any = def.has("power");
        p.transmitter = def.flag("power.transmitter");
        p.switchable = def.flag("power.switchable");
        p.output = std::max(0.0, def.number("power.output"));
        p.sun = def.flag("power.scalesWithSun");
        p.wind = def.flag("power.scalesWithWind");
        p.windBlockRadius = std::max(0, static_cast<int>(def.integer("windBlockRadius")));
        p.geyser = def.flag("power.needsGeyser");
        p.dryRadius = std::max(0, static_cast<int>(def.integer("dries.radius")));
        p.dryEvery = std::max(1, static_cast<int>(def.integer("dries.everySeconds")));
        p.fuelled = p.output > 0.0 && def.has("fuel");
        p.consumption = std::max(0.0, def.number("power.consumption"));
        p.battery = std::max(0.0, def.number("power.battery"));
        p.idleDrain = std::max(0.0, def.number("power.idleDrain"));
    }
    return infos_[id.value];
}

const PowerInfo& PowerGrid::infoOf(Entity building) const {
    const entt::registry& reg = things_.registry();
    static const PowerInfo none;
    if (!reg.valid(building) || !reg.all_of<Thing>(building)) {
        return none;
    }
    return info(reg.get<Thing>(building).def);
}

const PowerPart* PowerGrid::partOf(Entity building) const {
    const entt::registry& reg = things_.registry();
    return reg.valid(building) ? reg.try_get<PowerPart>(building) : nullptr;
}

void PowerGrid::update() {
    if (!dirty_ && buildingChanges_ == map_.buildingChanges()) {
        return;
    }
    rebuild();
}

void PowerGrid::rebuild() {
    ++rebuilds_;
    dirty_ = false;
    buildingChanges_ = map_.buildingChanges();
    entt::registry& reg = things_.registry();

    std::vector<Entity> found;
    for (const Entity e : reg.view<const PowerPart, const Footprint, const Position, const Thing>()) {
        found.push_back(e);
    }
    std::sort(found.begin(), found.end(), [](Entity a, Entity b) { return entt::to_integral(a) < entt::to_integral(b); });

    members_.clear();
    members_.reserve(found.size());
    for (const Entity e : found) {
        Member m;
        m.entity = e;
        m.info = &info(reg.get<Thing>(e).def);
        const CellRect rect = reg.get<Footprint>(e).rect();
        for (int y = rect.minY; y < rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX; x < rect.minX + rect.sizeX; ++x) {
                m.cells.push_back(CellPos{x, y, rect.level});
            }
        }
        members_.push_back(std::move(m));
    }

    // Conduits and closed switches, by cell. An open switch joins nothing.
    const uint32_t count = static_cast<uint32_t>(members_.size());
    std::unordered_map<uint64_t, uint32_t> transmitterAt;
    for (uint32_t i = 0; i < count; ++i) {
        const Member& m = members_[i];
        const PowerPart& part = reg.get<PowerPart>(m.entity);
        if (m.info->transmitter && (part.on || !m.info->switchable)) {
            for (const CellPos c : m.cells) {
                transmitterAt[powerCellKey(c)] = i;
            }
        }
    }

    // Joins: a transmitter to the transmitters on its four sides; any other building to the
    // transmitters under it or on the four sides of its edge cells.
    static const int kSides[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    std::vector<uint32_t> parent(count);
    std::iota(parent.begin(), parent.end(), 0u);
    std::vector<uint8_t> linked(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        const Member& m = members_[i];
        const PowerPart& part = reg.get<PowerPart>(m.entity);
        const bool joins = m.info->transmitter ? (part.on || !m.info->switchable) : true;
        if (!joins) {
            continue;
        }
        const auto tryJoin = [&](CellPos c) {
            const auto it = transmitterAt.find(powerCellKey(c));
            if (it != transmitterAt.end() && it->second != i) {
                powerUnion(parent, i, it->second);
                linked[i] = 1;
                linked[it->second] = 1;
            }
        };
        for (const CellPos c : m.cells) {
            if (!m.info->transmitter) {
                tryJoin(c);  // a conduit under it, were that possible
            }
            for (const auto& side : kSides) {
                tryJoin(CellPos{c.x + side[0], c.y + side[1], c.level});
            }
        }
    }

    // A net is a group of linked buildings with at least one that is not a conduit or a switch.
    std::vector<uint8_t> hasUser(count, 0);
    for (uint32_t i = 0; i < count; ++i) {
        if (linked[i] != 0 && !members_[i].info->transmitter) {
            hasUser[powerFind(parent, i)] = 1;
        }
    }
    std::vector<uint32_t> netOfRoot(count, 0);
    netMembers_.clear();
    std::vector<uint8_t> wasSatisfied;
    for (uint32_t i = 0; i < count; ++i) {
        PowerPart& part = reg.get<PowerPart>(members_[i].entity);
        part.net = 0;
        if (linked[i] == 0) {
            continue;
        }
        const uint32_t root = powerFind(parent, i);
        if (hasUser[root] == 0) {
            continue;
        }
        if (netOfRoot[root] == 0) {
            netMembers_.emplace_back();
            netOfRoot[root] = static_cast<uint32_t>(netMembers_.size());
            wasSatisfied.push_back(0);
        }
        part.net = netOfRoot[root];
        netMembers_[part.net - 1].members.push_back(i);
        if (part.powered) {
            wasSatisfied[part.net - 1] = 1;
        }
    }
    nets_.assign(netMembers_.size(), PowerNetView{});
    for (size_t n = 0; n < nets_.size(); ++n) {
        nets_[n].id = static_cast<uint32_t>(n + 1);
        nets_[n].satisfied = wasSatisfied[n] != 0;
    }

    // How open to the wind each turbine is (checked again every game hour).
    windOpen_.assign(count, 1.0);
    for (uint32_t i = 0; i < count; ++i) {
        if (members_[i].info->wind) {
            windOpen_[i] = windOpenShare(members_[i]);
        }
    }
}

double PowerGrid::sunNow(const GameTime& time) const {
    if (sun_) {
        return std::clamp(sun_(time), 0.0, 1.0);
    }
    const double hour = static_cast<double>(time.seconds % kSecondsPerDay) / static_cast<double>(kSecondsPerHour);
    return powerSmoothStep(numbers_.sunrise, numbers_.fullFrom, hour) *
           (1.0 - powerSmoothStep(numbers_.fullTo, numbers_.sunset, hour));
}

// The share of a generator's footprint with no roof over it.
double PowerGrid::sunFactor(const Member& m) const {
    if (m.cells.empty()) {
        return 0.0;
    }
    int open = 0;
    for (const CellPos c : m.cells) {
        if (map_.onGround(c) && map_.roof(c) == Roof::None) {
            ++open;
        }
    }
    return static_cast<double>(open) / static_cast<double>(m.cells.size());
}

// A turbine needs clear air at both ends of its long side: `windBlockRadius` cells deep and as wide
// as the turbine. A cell is blocked by a roof, rock or anything that stands there and is not low.
double PowerGrid::windOpenShare(const Member& m) const {
    if (m.cells.empty() || m.info->windBlockRadius <= 0) {
        return 1.0;
    }
    int minX = m.cells.front().x;
    int maxX = minX;
    int minY = m.cells.front().y;
    int maxY = minY;
    for (const CellPos c : m.cells) {
        minX = std::min(minX, c.x);
        maxX = std::max(maxX, c.x);
        minY = std::min(minY, c.y);
        maxY = std::max(maxY, c.y);
    }
    const int level = m.cells.front().level;
    const bool alongX = (maxX - minX) >= (maxY - minY);
    const int radius = m.info->windBlockRadius;
    int total = 0;
    int blocked = 0;
    const auto look = [&](int x, int y) {
        const CellPos c{x, y, level};
        if (!map_.onGround(c)) {
            return;  // off the map is open sky
        }
        ++total;
        if (map_.roof(c) != Roof::None || map_.obstacleDm(c) > 0) {
            ++blocked;
        }
    };
    for (int step = 1; step <= radius; ++step) {
        if (alongX) {
            for (int y = minY; y <= maxY; ++y) {
                look(minX - step, y);
                look(maxX + step, y);
            }
        } else {
            for (int x = minX; x <= maxX; ++x) {
                look(x, minY - step);
                look(x, maxY + step);
            }
        }
    }
    return total == 0 ? 1.0 : 1.0 - static_cast<double>(blocked) / static_cast<double>(total);
}

void PowerGrid::step(const GameTime& time) {
    update();
    // The wind's openness changes when roofs do, which is not a building change.
    const int64_t hour = time.seconds / kSecondsPerHour;
    if (hour != windCheckedHour_) {
        windCheckedHour_ = hour;
        for (uint32_t i = 0; i < members_.size(); ++i) {
            if (members_[i].info->wind) {
                windOpen_[i] = windOpenShare(members_[i]);
            }
        }
    }
    balance(time);
    dry(time);
}

// A powered moisture pump turns the nearest still water cell within its reach into soil, one per
// `dries.everySeconds`. Moving water is left alone (docs/design/power.md).
void PowerGrid::dry(const GameTime& time) {
    const entt::registry& reg = things_.registry();
    for (const Member& m : members_) {
        if (m.info->dryRadius <= 0 || time.seconds % m.info->dryEvery != 0 || m.cells.empty() ||
            !reg.get<PowerPart>(m.entity).powered) {
            continue;
        }
        double cx = 0.0;
        double cy = 0.0;
        for (const CellPos c : m.cells) {
            cx += c.x;
            cy += c.y;
        }
        cx = cx / static_cast<double>(m.cells.size());
        cy = cy / static_cast<double>(m.cells.size());
        const int r = m.info->dryRadius;
        CellPos best{};
        double bestD = 0.0;
        bool found = false;
        for (int y = static_cast<int>(cy) - r; y <= static_cast<int>(cy) + r; ++y) {
            for (int x = static_cast<int>(cx) - r; x <= static_cast<int>(cx) + r; ++x) {
                const CellPos c{x, y, 0};
                if (!map_.onGround(c) || map_.terrain(c) != Terrain::Water || map_.flowing(c)) {
                    continue;
                }
                const double d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d <= static_cast<double>(r) * r && (!found || d < bestD)) {
                    best = c;
                    bestD = d;
                    found = true;
                }
            }
        }
        if (found) {
            map_.dryWater(best);
        }
    }
}

void PowerGrid::balance(const GameTime& time) {
    entt::registry& reg = things_.registry();
    const double sun = sunNow(time);
    const double wind = wind_ ? std::max(0.0, wind_(time)) : numbers_.windDefault;

    for (size_t n = 0; n < netMembers_.size(); ++n) {
        PowerNetView& view = nets_[n];
        view.generation = 0.0;
        view.demand = 0.0;
        view.stored = 0.0;
        view.capacity = 0.0;
        view.generators = 0;
        view.consumers = 0;
        view.batteries = 0;
        double idleDrain = 0.0;
        for (const uint32_t i : netMembers_[n].members) {
            const Member& m = members_[i];
            PowerPart& part = reg.get<PowerPart>(m.entity);
            part.watts = 0.0;
            if (m.info->transmitter) {
                continue;
            }
            if (m.info->output > 0.0) {
                ++view.generators;
                double factor = part.on ? 1.0 : 0.0;
                if (m.info->sun) {
                    factor *= sun * sunFactor(m);
                }
                if (m.info->wind) {
                    factor *= wind * windOpen_[i];
                }
                if (m.info->geyser) {
                    bool vents = false;
                    for (const CellPos c : m.cells) {
                        vents = vents || map_.geyser(c);
                    }
                    factor *= vents ? 1.0 : 0.0;
                }
                if (m.info->fuelled && fuel_ && !fuel_(m.entity)) {
                    factor = 0.0;
                }
                part.watts = m.info->output * factor;
                view.generation += part.watts;
            }
            if (m.info->consumption > 0.0) {
                ++view.consumers;
                if (part.on) {
                    const double wanted = part.draw >= 0.0 ? part.draw : m.info->consumption;
                    view.demand += wanted;
                    part.watts = -wanted;
                }
            }
            if (m.info->battery > 0.0) {
                ++view.batteries;
                view.capacity += m.info->battery;
                view.stored += part.stored;
                idleDrain += m.info->idleDrain;
            }
        }
        view.demand += idleDrain;

        if (blackout_) {
            // A solar flare: nothing generates and nothing is drawn from the batteries, so the whole net is dark.
            view.generation = 0.0;
            view.satisfied = false;
            for (const uint32_t i : netMembers_[n].members) {
                reg.get<PowerPart>(members_[i].entity).watts = 0.0;
            }
            continue;
        }

        // Who is served this step. A net that can't cover what its consumers want, from the
        // generators and the batteries, goes dark as a whole, as RimWorld's does. Once dark it waits
        // until its batteries hold `restartReserveSeconds` of the shortfall, so it doesn't flicker.
        const double shortfallWd = (view.demand - view.generation) * kPowerDaysPerStep;
        if (shortfallWd <= 0.0) {
            view.satisfied = true;
        } else if (view.satisfied) {
            view.satisfied = view.stored >= shortfallWd;
        } else {
            view.satisfied = view.stored >= shortfallWd * std::max(1.0, numbers_.restartReserveSeconds);
        }

        // Energy: a surplus charges batteries (keeping `batteryEfficiency` of it), a shortfall
        // comes out of them. In a dark net the consumers are off, so only the idle drain counts.
        double balanceWd = (view.generation - (view.satisfied ? view.demand : idleDrain)) * kPowerDaysPerStep;
        for (const uint32_t i : netMembers_[n].members) {
            const Member& m = members_[i];
            if (m.info->battery <= 0.0 || balanceWd == 0.0) {
                continue;
            }
            PowerPart& part = reg.get<PowerPart>(m.entity);
            if (balanceWd > 0.0) {
                const double room = m.info->battery - part.stored;
                const double kept = std::min(room, balanceWd * numbers_.batteryEfficiency);
                if (kept > 0.0) {
                    part.stored += kept;
                    balanceWd -= kept / numbers_.batteryEfficiency;
                }
            } else {
                const double taken = std::min(part.stored, -balanceWd);
                part.stored -= taken;
                balanceWd += taken;
            }
        }
        view.stored = 0.0;
        for (const uint32_t i : netMembers_[n].members) {
            if (members_[i].info->battery > 0.0) {
                view.stored += reg.get<PowerPart>(members_[i].entity).stored;
            }
        }
        if (!view.satisfied) {
            for (const uint32_t i : netMembers_[n].members) {
                if (members_[i].info->consumption > 0.0) {
                    reg.get<PowerPart>(members_[i].entity).watts = 0.0;
                }
            }
        }
    }

    // Everyone's powered flag, and an event for each building whose flag changed.
    bool anyChanged = false;
    for (uint32_t i = 0; i < members_.size(); ++i) {
        const Member& m = members_[i];
        PowerPart& part = reg.get<PowerPart>(m.entity);
        const bool now = part.net != 0 && nets_[part.net - 1].satisfied && part.on;
        if (now != part.powered) {
            part.powered = now;
            anyChanged = true;
            if (m.info->transmitter) {
                continue;  // conduits and switches would only add noise
            }
            Event event;
            event.type = EventType::PowerChanged;
            event.thing = m.entity;
            event.def = reg.get<Thing>(m.entity).def;
            event.cell = reg.get<Footprint>(m.entity).origin;
            event.message = now ? "on" : "off";
            events_.push_back(std::move(event));
        }
    }
    if (anyChanged && onChange_) {
        onChange_();
    }
}

bool PowerGrid::hasPower(Entity building) const {
    return partOf(building) != nullptr;
}

bool PowerGrid::powered(Entity building) const {
    const PowerPart* part = partOf(building);
    return part != nullptr && part->powered && part->on;
}

bool PowerGrid::running(Entity building) const {
    const PowerPart* part = partOf(building);
    if (part == nullptr) {
        return true;
    }
    if (infoOf(building).consumption <= 0.0) {
        return true;
    }
    return part->on && part->powered;
}

PowerState PowerGrid::stateOf(Entity building) const {
    const PowerPart* part = partOf(building);
    if (part == nullptr) {
        return PowerState::None;
    }
    if (!part->on) {
        return PowerState::Off;
    }
    return part->powered ? PowerState::Powered : PowerState::Unpowered;
}

uint32_t PowerGrid::netOf(Entity building) const {
    const PowerPart* part = partOf(building);
    return part != nullptr ? part->net : 0;
}

const PowerNetView* PowerGrid::net(uint32_t id) const {
    return id >= 1 && id <= nets_.size() ? &nets_[id - 1] : nullptr;
}

std::vector<Entity> PowerGrid::membersOf(uint32_t id) const {
    std::vector<Entity> out;
    if (id >= 1 && id <= netMembers_.size()) {
        for (const uint32_t i : netMembers_[id - 1].members) {
            out.push_back(members_[i].entity);
        }
    }
    return out;
}

double PowerGrid::wattsOf(Entity building) const {
    const PowerPart* part = partOf(building);
    return part != nullptr ? part->watts : 0.0;
}

double PowerGrid::storedIn(Entity battery) const {
    const PowerPart* part = partOf(battery);
    return part != nullptr ? part->stored : 0.0;
}

double PowerGrid::capacityOf(Entity battery) const {
    return infoOf(battery).battery;
}

bool PowerGrid::flick(Entity building, bool on, std::string& error) {
    PowerPart* part = things_.registry().valid(building) ? things_.registry().try_get<PowerPart>(building) : nullptr;
    if (part == nullptr) {
        error = "that building has no power to switch";
        return false;
    }
    const PowerInfo& p = infoOf(building);
    if (p.transmitter && !p.switchable) {
        error = "a conduit has no switch";
        return false;
    }
    if (p.battery > 0.0 && p.output <= 0.0 && p.consumption <= 0.0) {
        error = "a battery has no switch";
        return false;
    }
    if (part->on != on) {
        part->on = on;
        dirty_ = true;  // a switch joins or splits nets
    }
    return true;
}

bool PowerGrid::setDraw(Entity building, double watts) {
    PowerPart* part = things_.registry().valid(building) ? things_.registry().try_get<PowerPart>(building) : nullptr;
    if (part == nullptr || infoOf(building).consumption <= 0.0) {
        return false;
    }
    part->draw = watts < 0.0 ? -1.0 : watts;
    return true;
}

bool PowerGrid::shortCircuit(Entity at, double& released, std::string& error) {
    entt::registry& reg = things_.registry();
    released = 0.0;
    PowerPart* part = reg.valid(at) ? reg.try_get<PowerPart>(at) : nullptr;
    if (part == nullptr || !reg.all_of<Footprint>(at)) {
        error = "that building has no power to short out";
        return false;
    }
    update();
    if (part->net != 0) {
        for (const uint32_t i : netMembers_[part->net - 1].members) {
            if (members_[i].info->battery > 0.0) {
                PowerPart& battery = reg.get<PowerPart>(members_[i].entity);
                released += battery.stored;
                battery.stored = 0.0;
            }
        }
        nets_[part->net - 1].stored = 0.0;
    }
    Event event;
    event.type = EventType::ShortCircuit;
    event.thing = at;
    event.def = reg.get<Thing>(at).def;
    event.cell = reg.get<Footprint>(at).origin;
    event.amount = static_cast<float>(released);
    events_.push_back(std::move(event));
    return true;
}

}  // namespace colony

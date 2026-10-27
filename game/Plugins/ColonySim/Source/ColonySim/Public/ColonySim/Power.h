#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace colony {

class Map;
class Rooms;
class Things;

// Power, as RimWorld's: conduits join the buildings beside them into nets; generators feed a net,
// consumers draw from it, batteries buffer it, and a net that can't cover its consumers goes dark.
// See docs/design/power.md.
//
// Units are RimWorld's: watts for output and draw, watt-days (Wd) for stored energy. A game day is
// 86,400 steps, so a step moves `watts / 86400` Wd.

// What the snapshot says about a building's power.
enum class PowerState : uint8_t {
    None,       // the building has nothing to do with power
    Off,        // switched off (a switch that is open, or a consumer that was flicked off)
    Unpowered,  // on, but not in a net that is working (not wired, or the net is short)
    Powered,    // on, in a net that is working
};

// On every building whose def has a `power` object (Things.cpp adds it). Only the state a save
// needs; what a def says about the building is read through PowerGrid::info.
struct PowerPart {
    bool on = true;        // false: a switch open or a consumer flicked off (FlickCommand)
    bool powered = false;  // in a net that is working, as of the last step
    double stored = 0.0;   // batteries: energy held, Wd
    double draw = -1.0;    // consumers: watts wanted right now; below 0 the def's `power.consumption`

    // Worked out again every step or when buildings change; not saved, not hashed.
    uint32_t net = 0;      // 1, 2, 3... in id order of the net's lowest member; 0: not in a net
    double watts = 0.0;    // + generating now, - drawing now
};

// What a net is doing, for panels and overlays.
struct PowerNetView {
    uint32_t id = 0;
    double generation = 0.0;  // W
    double demand = 0.0;      // W, consumers that are on plus batteries' idle drain
    double stored = 0.0;      // Wd in all batteries
    double capacity = 0.0;    // Wd
    bool satisfied = true;
    int generators = 0;
    int consumers = 0;
    int batteries = 0;
};

// The numbers in the PowerRules def (with these values when the def is missing).
struct COLONY_API PowerNumbers {
    double batteryEfficiency = 0.5;     // share of energy put into a battery that is kept
    double restartReserveSeconds = 60;  // a net that went dark comes back when its batteries hold this many seconds of its shortfall
    // Sun over the day (hour of day): none before `sunrise`, rising to full at `fullFrom`, full to
    // `fullTo`, falling to none at `sunset`. E3's light grid replaces this through setSun.
    double sunrise = 5.0;
    double fullFrom = 8.0;
    double fullTo = 16.0;
    double sunset = 19.0;
    double windDefault = 1.0;           // wind turbine output factor until the weather (E1) sets one

    static PowerNumbers read(const DefDatabase& defs);
};

// What a building's def says about power, read once per def.
struct PowerInfo {
    bool any = false;
    bool transmitter = false;  // a conduit or a switch: joins the buildings beside it
    bool switchable = false;   // a switch
    double output = 0.0;       // W of a generator
    bool sun = false;          // output follows the sun, and the share of its roof that is open
    bool wind = false;         // output follows the wind and what stands in its way
    int windBlockRadius = 0;
    bool geyser = false;       // output needs a steam geyser under the footprint (`power.needsGeyser`)
    bool fuelled = false;      // a generator that needs fuel (`fuel` object): see PowerGrid::setFuel
    double consumption = 0.0;  // W of a consumer
    double battery = 0.0;      // Wd capacity
    double idleDrain = 0.0;    // W a battery costs its net
};

class COLONY_API PowerGrid {
public:
    PowerGrid(Map& map, Things& things, Rooms& rooms, const DefDatabase& defs, std::vector<Event>& events);

    const PowerNumbers& numbers() const { return numbers_; }
    // What the def of `thing` says; `any` is false for things that have no `power` object.
    const PowerInfo& info(DefId def) const;

    // Where outside conditions come from. Until the systems that own them exist the defaults are a
    // sun curve from PowerNumbers, a constant wind and fuel that never runs out.
    //   sun:  0 to 1, how much sunlight there is right now (E3's light grid)
    //   wind: the wind turbine output factor, 0 and up (E1's weather)
    //   fuel: whether a generator that burns fuel has some (C11's refuelable part)
    void setSun(std::function<double(const GameTime&)> sun) { sun_ = std::move(sun); }
    void setWind(std::function<double(const GameTime&)> wind) { wind_ = std::move(wind); }
    void setFuel(std::function<bool(Entity)> fuel) { fuel_ = std::move(fuel); }
    // Called after a step in which any building gained or lost power (the light grid rebuilds its lamps).
    void setOnChange(std::function<void()> onChange) { onChange_ = std::move(onChange); }

    // Makes the nets again if a building was set or cleared or a switch flicked since the last
    // call. Cheap when nothing changed. The sim calls it at the end of loading and in every step.
    void update();
    // update(), then one game second of power: generation, draw, batteries, who has power.
    void step(const GameTime& time);
    void markDirty() { dirty_ = true; }

    // --- Asking ---
    // The building has a power part (a conduit, a lamp, a generator...).
    bool hasPower(Entity building) const;
    // Whether the building does its job: buildings that draw power need it on and in a working net;
    // others (conduits, generators, batteries, a stove that burns wood) are always true.
    bool running(Entity building) const;
    // Switched on and in a working net, for any building with a power part.
    bool powered(Entity building) const;
    PowerState stateOf(Entity building) const;
    // Net id of a building, 0 for none. Ids are good until the next change to the nets.
    uint32_t netOf(Entity building) const;
    const std::vector<PowerNetView>& nets() const { return nets_; }
    const PowerNetView* net(uint32_t id) const;
    // The buildings in a net, in entity id order; empty for an unknown id.
    std::vector<Entity> membersOf(uint32_t id) const;
    // Current output of a generator or draw of a consumer, W (positive generating).
    double wattsOf(Entity building) const;
    // Energy in a battery, Wd; its capacity.
    double storedIn(Entity battery) const;
    double capacityOf(Entity battery) const;
    uint64_t rebuilds() const { return rebuilds_; }

    // --- Doing ---
    // Opens or closes a switch, or flicks a consumer on or off. False, with `error`, if the
    // building can't be switched.
    bool flick(Entity building, bool on, std::string& error);
    // Sets the watts a consumer wants right now (a heater that has reached its temperature asks for
    // less); a negative value goes back to the def's consumption. Systems that own the building's
    // job call it.
    bool setDraw(Entity building, double watts);
    // A short circuit at `at` (a conduit or any building on the net): every battery on the net is
    // emptied and the conduit or building burns out (T3, docs/design/power.md). Returns the energy
    // released in Wd (0 if `at` is not on a net with a battery, then nothing but the conduit
    // happens); a ShortCircuit event carries it for the explosion and fire that follow (F9, E4).
    // False, with `error`, if `at` has no power part.
    bool shortCircuit(Entity at, double& released, std::string& error);
    // A solar flare (T3): while on, no generator makes power and no battery gives any; every net is dark. Not saved:
    // the disaster system that owns the flare sets it again every step.
    void setBlackout(bool on) { blackout_ = on; }
    bool blackout() const { return blackout_; }

private:
    struct Member {
        Entity entity = kNoEntity;
        const PowerInfo* info = nullptr;
        std::vector<CellPos> cells;  // footprint
    };
    struct Net {
        std::vector<uint32_t> members;  // indexes into members_
    };

    void rebuild();
    void balance(const GameTime& time);
    double sunNow(const GameTime& time) const;
    double sunFactor(const Member& m) const;
    double windFactor(const Member& m, double wind);
    double windOpenShare(const Member& m) const;
    const PowerInfo& infoOf(Entity building) const;
    const PowerPart* partOf(Entity building) const;

    Map& map_;
    Things& things_;
    Rooms& rooms_;
    const DefDatabase& defs_;
    std::vector<Event>& events_;
    PowerNumbers numbers_;
    mutable std::vector<PowerInfo> infos_;  // per def id, filled on first ask
    mutable std::vector<uint8_t> infoKnown_;
    std::function<double(const GameTime&)> sun_;
    std::function<double(const GameTime&)> wind_;
    std::function<bool(Entity)> fuel_;
    std::function<void()> onChange_;

    bool dirty_ = true;
    bool blackout_ = false;
    uint64_t buildingChanges_ = 0;
    uint64_t rebuilds_ = 0;
    std::vector<Member> members_;           // buildings with power parts, in entity id order
    std::vector<Net> netMembers_;           // by net id - 1
    std::vector<PowerNetView> nets_;        // by net id - 1
    std::vector<double> windOpen_;          // per member, share of the turbine's wind cells that are clear
    int64_t windCheckedHour_ = -1;
};

}  // namespace colony

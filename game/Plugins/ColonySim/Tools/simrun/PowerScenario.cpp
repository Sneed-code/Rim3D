#include "PowerScenario.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Map.h"
#include "ColonySim/Power.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace powerscenario {

namespace {

using namespace colony;

constexpr int kLamps = 10;

int powerFail(const char* what) {
    std::fprintf(stderr, "power check failed: %s\n", what);
    return 1;
}

}  // namespace

int run(double days, std::shared_ptr<const DefDatabase> defs, bool verbose) {
    if (!defs) {
        std::fprintf(stderr, "--power needs the game's defs\n");
        return 2;
    }
    Sim sim(SimSettings{1, 96, 0, 0, true}, defs);
    Things& things = sim.things();
    const auto put = [&](const char* def, int x, int y) { return things.spawn(def, CellPos{x, y, 0}); };

    // Row y = 40 is the main wire, x from 20 to 60. Panel and generator sit above it, batteries and
    // lamps below, a switch splits off the heater on the east end.
    for (int x = 20; x <= 60; ++x) {
        if (x != 50) {
            put("PowerConduit", x, 40);
        }
    }
    const Entity sw = put("PowerSwitch", 50, 40);
    const Entity panel = put("SolarGenerator", 20, 41);
    const Entity wood = put("WoodFiredGenerator", 26, 41);
    const Entity batteryA = put("Battery", 30, 38);
    const Entity batteryB = put("Battery", 32, 38);
    std::vector<Entity> lamps;
    for (int i = 0; i < kLamps; ++i) {
        lamps.push_back(put("StandingLamp", 34 + i, 41));
    }
    const Entity heater = put("Heater", 61, 40);
    if (panel == kNoEntity || wood == kNoEntity || batteryA == kNoEntity || batteryB == kNoEntity || sw == kNoEntity ||
        heater == kNoEntity || std::count(lamps.begin(), lamps.end(), kNoEntity) > 0) {
        std::fprintf(stderr, "the power check could not place its buildings\n");
        return 2;
    }
    std::string error;
    sim.power().flick(wood, false, error);  // the sun and the batteries have to do it alone

    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    int lampOutageHours = 0;
    int heaterOutageHours = 0;
    double stored6 = -1.0;   // at the start of day 1, 06:00
    double stored17 = -1.0;  // at day 1, 17:00
    double stored5next = -1.0;
    double peak = 0.0;
    bool switchChecked = false;
    if (verbose) {
        std::fprintf(stderr, "hour  generation  demand  stored   lamps  heater\n");
    }
    for (int64_t s = 0; s < steps; ++s) {
        sim.step();
        if (s % kSecondsPerHour != 0) {
            continue;
        }
        const int64_t hour = s / kSecondsPerHour;
        const PowerNetView* net = sim.power().net(sim.power().netOf(panel));
        if (net == nullptr) {
            return powerFail("the panel is on no net");
        }
        const bool lampsOn = sim.power().powered(lamps.front());
        const bool heaterOn = sim.power().powered(heater);
        peak = std::max(peak, net->stored);
        if (hour == 30) {
            stored6 = net->stored;
        }
        if (hour == 41) {
            stored17 = net->stored;
        }
        if (hour == 53) {
            stored5next = net->stored;
        }
        if (hour >= 12 && !lampsOn) {
            ++lampOutageHours;
        }
        if (hour >= 12 && !heaterOn) {
            ++heaterOutageHours;
        }
        if (verbose) {
            std::fprintf(stderr, "%4lld  %9.0f  %6.0f  %7.2f  %-5s  %s\n", static_cast<long long>(hour), net->generation,
                         net->demand, net->stored, lampsOn ? "on" : "OFF", heaterOn ? "on" : "OFF");
        }
        // Once, in the second day's morning: open the switch and see the heater drop off alone.
        if (!switchChecked && hour == 36) {
            sim.submit(FlickCommand{sw, false});
            sim.step();
            if (sim.power().powered(heater)) {
                return powerFail("the heater kept power with the switch open");
            }
            if (!sim.power().powered(lamps.front())) {
                return powerFail("the lamps lost power when the switch beyond them opened");
            }
            sim.submit(FlickCommand{sw, true});
            sim.step();
            if (!sim.power().powered(heater)) {
                return powerFail("the heater did not come back when the switch closed");
            }
            switchChecked = true;
        }
    }
    std::printf(
        "{\"power\":{\"days\":%.2f,\"lampOutageHoursAfterNoon\":%d,\"heaterOutageHoursAfterNoon\":%d,\"peakStoredWd\":%.2f,"
        "\"stored06\":%.2f,\"stored17\":%.2f,\"nets\":%zu}}\n",
        days, lampOutageHours, heaterOutageHours, peak, stored6, stored17, sim.power().nets().size());
    if (days < 2.1) {
        return 0;  // too short for the day and night checks
    }
    if (lampOutageHours > 0) {
        return powerFail("the lamps went dark after the first noon");
    }
    if (heaterOutageHours > 0) {
        return powerFail("the heater went dark after the first noon");
    }
    if (!(stored17 > stored6)) {
        return powerFail("the batteries did not charge during the day");
    }
    if (!(stored5next < stored17)) {
        return powerFail("the batteries did not fall during the night");
    }
    if (!switchChecked) {
        return powerFail("the switch was never tried");
    }
    return 0;
}

}  // namespace powerscenario

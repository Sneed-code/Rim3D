#include "PerfScenario.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Events.h"
#include "ColonySim/PerfColony.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace perfscenario {

using namespace colony;

int run(uint64_t seed, int mapSize, double days, double budgetMs, std::shared_ptr<const DefDatabase> defs) {
    using Clock = std::chrono::steady_clock;
    const auto ms = [](auto from, auto to) { return std::chrono::duration<double, std::milli>(to - from).count(); };

    SimSettings settings{seed, mapSize, 0, 0};
    settings.scenario = "PerfColony";
    settings.wildAnimals = 100;
    const auto start = Clock::now();
    Sim sim(settings, defs);
    const auto generated = Clock::now();
    PerfColonyCounts counts;
    if (const std::string error = setUpPerfColony(sim, counts); !error.empty()) {
        for (const std::string& e : sim.setupErrors()) {
            std::fprintf(stderr, "%s\n", e.c_str());
        }
        std::fprintf(stderr, "perf: %s\n", error.c_str());
        return 2;
    }
    const int colonists = counts.colonists;
    const int penned = counts.tameAnimals;
    const int wild = counts.wildAnimals;

    StepProfile profile;
    sim.setProfile(&profile);
    const int64_t steps = static_cast<int64_t>(days * static_cast<double>(kSecondsPerDay));
    std::vector<float> stepMs(static_cast<size_t>(steps));
    uint64_t commandsFailed = 0;
    float worstMs = 0.0f;
    int64_t worstStep = -1;
    const char* worstSystem = "?";
    const auto run0 = Clock::now();
    for (int64_t i = 0; i < steps; ++i) {
        if (sim.time().seconds % kSecondsPerHour == 0) {
            feedPerfColonyAnimals(sim);
        }
        const std::vector<double> lapsBefore = profile.ms;
        const auto before = Clock::now();
        sim.step();
        stepMs[static_cast<size_t>(i)] = static_cast<float>(ms(before, Clock::now()));
        if (stepMs[static_cast<size_t>(i)] > worstMs) {
            // Which system took the time in the dearest step so far.
            worstMs = stepMs[static_cast<size_t>(i)];
            worstStep = i;
            worstSystem = "?";
            double most = 0.0;
            for (size_t k = 0; k < profile.names.size(); ++k) {
                const double spent = profile.ms[k] - (k < lapsBefore.size() ? lapsBefore[k] : 0.0);
                if (std::string(profile.names[k]) != "total" && spent > most) {
                    most = spent;
                    worstSystem = profile.names[k];
                }
            }
        }
        for (const Event& e : sim.events()) {
            if (e.type == EventType::CommandFailed) {
                ++commandsFailed;
                std::fprintf(stderr, "step %lld: command %llu failed: %s\n", static_cast<long long>(i),
                             static_cast<unsigned long long>(e.command), e.message.c_str());
            }
        }
    }
    const double runMs = ms(run0, Clock::now());
    sim.setProfile(nullptr);

    double total = 0.0;
    for (const float v : stepMs) {
        total += v;
    }
    const double avg = steps > 0 ? total / static_cast<double>(steps) : 0.0;
    std::vector<float> sorted = stepMs;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double p) {
        return sorted.empty() ? 0.0 : static_cast<double>(sorted[static_cast<size_t>(p * static_cast<double>(sorted.size() - 1))]);
    };
    // At 3x the sim runs 90 steps per real second; at 60 fps that is 1.5 steps per frame.
    const double msPerFrameAt3x = avg * 3.0 * kStepsPerRealSecondAt1x / 60.0;

    // The table: systems by cost, with their share of the whole step.
    std::vector<std::pair<double, const char*>> rows;
    for (size_t i = 0; i < profile.names.size(); ++i) {
        if (std::string(profile.names[i]) != "total") {
            rows.emplace_back(profile.ms[i], profile.names[i]);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::fprintf(stderr, "%-12s %10s %7s\n", "system", "ms/step", "share");
    for (const auto& [cost, name] : rows) {
        std::fprintf(stderr, "%-12s %10.4f %6.1f%%\n", name, cost / static_cast<double>(steps), total > 0.0 ? 100.0 * cost / total : 0.0);
    }

    std::printf("{\"perf\":{\"seed\":%llu,\"size\":%d,\"days\":%.2f,\"colonists\":%d,\"tameAnimals\":%d,\"wildAnimals\":%d,"
                "\"things\":%zu,\"mapGenMs\":%.1f,\"runMs\":%.1f,\"avgStepMs\":%.4f,\"p99StepMs\":%.3f,\"maxStepMs\":%.3f,"
                "\"msPerFrameAt3x\":%.3f,\"worstStep\":%lld,\"worstSystem\":\"%s\",\"commandsFailed\":%llu,\"stateHash\":\"%016llx\",\"systems\":{",
                static_cast<unsigned long long>(seed), mapSize, days, colonists, penned, wild, sim.things().count(),
                ms(start, generated), runMs, avg, percentile(0.99), percentile(1.0), msPerFrameAt3x,
                static_cast<long long>(worstStep), worstSystem, static_cast<unsigned long long>(commandsFailed),
                static_cast<unsigned long long>(sim.stateHash()));
    for (size_t i = 0; i < rows.size(); ++i) {
        std::printf("%s\"%s\":%.4f", i == 0 ? "" : ",", rows[i].second, rows[i].first / static_cast<double>(steps));
    }
    std::printf("}}}\n");

    if (budgetMs > 0.0 && avg > budgetMs) {
        std::fprintf(stderr, "perf: the average step costs %.4f ms, over the budget of %.4f ms\n", avg, budgetMs);
        return 1;
    }
    return 0;
}

}  // namespace perfscenario

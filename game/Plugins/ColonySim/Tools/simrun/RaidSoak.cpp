#include "RaidSoak.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace raidsoak {

namespace {

constexpr int kStuckSteps = 150;      // a fighter that should be walking but gets nowhere this long is stuck
constexpr float kStuckMetres = 0.5f;  // getting nowhere: less than this far from where it was 150 steps ago
constexpr double kOpenLimit = 10.0;   // percent: the design rule
constexpr int kOneIn = 20;            // stuck fighters and raids still on after a quarter day: more than one seed in this many fails
constexpr int kFirefightSteps = 2400;
constexpr int kRaidSteps = 21600;    // a quarter of a game day, as the CTest raid

struct Tally {
    int64_t waiting = 0;   // pawn-steps waiting in place with an enemy known
    int64_t inOpen = 0;    // of those, in the open from the nearest enemy
    int downed = 0;
    int died = 0;
    int stuck = 0;         // fighters that got stuck, counted once each
    int64_t fightSteps = 0;
};

// Counts fighters that should be walking (their plan says so) but get nowhere: under half a metre in 150 steps.
// A hurt pawn on a steep slope is slow and still gets somewhere; one held up for good does not.
class StuckWatch {
public:
    void watch(colony::Sim& sim) {
        for (const auto [pawn, f] : sim.things().registry().view<const colony::Fighter>().each()) {
            if (sim.health().incapacitated(pawn) || f.walk == 0) {
                seen_.erase(pawn);
                continue;
            }
            const colony::Position& pos = sim.things().registry().get<colony::Position>(pawn);
            Seen& s = seen_[pawn];
            const float dx = pos.x - s.x;
            const float dy = pos.y - s.y;
            if (s.steps == 0 || dx * dx + dy * dy >= kStuckMetres * kStuckMetres) {
                s = Seen{pos.x, pos.y, 1};
            } else if (++s.steps == kStuckSteps) {
                ++stuck_;
            }
        }
    }
    int stuck() const { return stuck_; }

private:
    struct Seen {
        float x = 0.0f;
        float y = 0.0f;
        int steps = 0;
    };
    std::map<colony::Entity, Seen> seen_;
    int stuck_ = 0;
};

bool upright(colony::Sim& sim, const std::vector<colony::Entity>& side) {
    return std::any_of(side.begin(), side.end(), [&](colony::Entity p) { return !sim.health().incapacitated(p); });
}

// Is there a cell within 10 m that is in cover from the enemy at (ex, ey) and that the fighter may stand on? A drafted
// fighter keeps within its leash of the place it was drafted, so cover beyond that is no use to it.
bool coverNear(colony::Sim& sim, const colony::Fighter& f, colony::CellPos at, float ex, float ey) {
    for (int dy = -10; dy <= 10; ++dy) {
        for (int dx = -10; dx <= 10; ++dx) {
            const colony::CellPos c{at.x + dx, at.y + dy, at.level};
            const float ax = static_cast<float>(c.x - f.anchor.x);
            const float ay = static_cast<float>(c.y - f.anchor.y);
            if (dx * dx + dy * dy <= 100 && (f.leash <= 0.0f || ax * ax + ay * ay <= f.leash * f.leash) && sim.map().inBounds(c) &&
                sim.cover().coverAgainst(c, colony::Stance::Crouching, ex, ey) >= 0.5f) {
                return true;
            }
        }
    }
    return false;
}

// Three against three across a field of scattered low cover. The cover's places come from the seed.
void firefight(uint64_t seed, const std::shared_ptr<const colony::DefDatabase>& defs, Tally& out, StuckWatch& watch) {
    colony::SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    colony::Sim sim(settings, defs);
    colony::Rng rng(seed, 77);
    for (const int rowBase : {36, 60}) {
        for (int line = 0; line < 3; ++line) {
            const int y = rowBase + static_cast<int>(rng.below(5));
            const int x0 = 26 + static_cast<int>(rng.below(30));
            const int len = 3 + static_cast<int>(rng.below(5));
            for (int x = x0; x < x0 + len; ++x) {
                sim.things().spawn("Table2x2", colony::CellPos{x, y, 0});
            }
        }
    }
    const char* guns[] = {"Gun_AssaultRifle", "Gun_BoltActionRifle", "Gun_Revolver"};
    std::vector<colony::Entity> ours;
    std::vector<colony::Entity> theirs;
    for (int i = 0; i < 3; ++i) {
        for (const bool hostile : {false, true}) {
            const int x = 30 + 7 * i + static_cast<int>(rng.below(3));
            const colony::Entity p = sim.things().spawn("Human", colony::CellPos{x, hostile ? 68 : 30, 0});
            sim.combat().equip(p, sim.things().make(sim.defs().idOf("Things", guns[hostile ? 2 - i : i])));
            sim.submit(hostile ? colony::Command{colony::SetFactionCommand{p, 2}} : colony::Command{colony::DraftCommand{p, true}});
            (hostile ? theirs : ours).push_back(p);
        }
    }
    int64_t end = kFirefightSteps;
    for (int64_t i = 0; i < kFirefightSteps; ++i) {
        sim.step();
        watch.watch(sim);
        for (const colony::Entity p : ours) {
            if (sim.health().incapacitated(p)) {
                continue;
            }
            const colony::Fighter& f = sim.things().registry().get<colony::Fighter>(p);
            const colony::Position& pos = sim.things().registry().get<colony::Position>(p);
            const colony::CellPos at = colony::cellOf(pos);
            if (f.walk != 0 || f.known.empty() || at != f.spot) {
                continue;
            }
            const colony::Shooter* sh = sim.things().registry().try_get<colony::Shooter>(p);
            if (sh != nullptr && (sh->phase == colony::ShotPhase::Aiming || sh->phase == colony::ShotPhase::Firing)) {
                continue;
            }
            float best = 1.0e9f;
            float ex = 0.0f;
            float ey = 0.0f;
            for (const colony::KnownEnemy& k : f.known) {
                const float d = (k.x - pos.x) * (k.x - pos.x) + (k.y - pos.y) * (k.y - pos.y);
                if (d < best && !sim.health().incapacitated(k.pawn)) {
                    best = d;
                    ex = k.x;
                    ey = k.y;
                }
            }
            if (best < 1.0e9f) {
                if (sim.cover().coverAgainst(at, colony::Stance::Crouching, ex, ey) >= 0.5f) {
                    ++out.waiting;
                } else if (coverNear(sim, f, at, ex, ey)) {
                    ++out.waiting;
                    ++out.inOpen;
                }  // in the open with no cover within 10 m is not the fighters' fault: not counted
            }
        }
        if (!upright(sim, ours) || !upright(sim, theirs)) {
            end = i + 1;
            break;
        }
    }
    out.fightSteps += end;
    for (const auto* side : {&ours, &theirs}) {
        for (const colony::Entity p : *side) {
            out.downed += sim.health().incapacitated(p) ? 1 : 0;
        }
    }
    for (const colony::Event& e : sim.events()) {
        out.died += e.type == colony::EventType::PawnDied ? 1 : 0;
    }
}

struct RaidResult {
    bool skipped = false;  // no colonists, or no way in from any edge: nothing to play
    bool arrived = false;
    bool ended = false;
    bool wiped = false;  // every colonist down or dead: the raid stays, nothing is stuck
    int64_t length = 0;  // game seconds from arrival to the end
    int colonistsDowned = 0;
    int colonistsDied = 0;
    int raidersDowned = 0;
    int raidersDied = 0;
    int tendedInFight = 0;  // with medics (F10): tends while the alarm was on
    int dragged = 0;        // downed colonists a fighter carried to cover
};

// `medics` (F10, docs/design/field-care.md): the first colonist is the alarm's Medic, the second its Firefighter.
RaidResult raid(uint64_t seed, const std::shared_ptr<const colony::DefDatabase>& defs, StuckWatch& watch, bool medics) {
    colony::SimSettings settings{seed, 256, 0, 0};
    settings.scenario = "Crashlanded";
    colony::Sim sim(settings, defs);
    RaidResult r;
    r.skipped = sim.things().registry().view<const colony::Colonist>().size() == 0;
    if (r.skipped) {
        return r;
    }
    for (const auto [pawn, colonist] : sim.things().registry().view<const colony::Colonist>().each()) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", "Gun_AssaultRifle")));
    }
    sim.submit(colony::StartRaidCommand{"Pirate", 300.0f, -1});
    if (medics) {
        std::vector<colony::Entity> crew(sim.things().registry().view<const colony::Colonist>().begin(),
                                         sim.things().registry().view<const colony::Colonist>().end());
        std::sort(crew.begin(), crew.end());
        for (size_t c = 0; c < crew.size() && c < 2; ++c) {
            const colony::DefenseRole role = c == 0 ? colony::DefenseRole::Medic : colony::DefenseRole::Firefighter;
            sim.submit(colony::SetDefenseRoleCommand{crew[c], static_cast<uint8_t>(role), false, colony::CellPos{}});
        }
    }
    int64_t arrivedAt = -1;
    for (int64_t i = 0; i < kRaidSteps && !r.ended; ++i) {
        sim.step();
        watch.watch(sim);
        for (const colony::Event& e : sim.events()) {
            const bool ours = e.thing != colony::kNoEntity && sim.things().exists(e.thing) &&
                              sim.fighters().factionOf(e.thing) == colony::kPlayerFaction;
            switch (e.type) {
                case colony::EventType::RaidArrived: r.arrived = true; arrivedAt = sim.time().seconds; break;
                case colony::EventType::PawnDowned: ++(ours ? r.colonistsDowned : r.raidersDowned); break;
                case colony::EventType::PawnDied: ++(ours ? r.colonistsDied : r.raidersDied); break;
                case colony::EventType::CommandFailed: r.skipped = !r.arrived; break;  // no way in from any edge: an island start
                case colony::EventType::PawnTended: r.tendedInFight += sim.raids().state().alarm ? 1 : 0; break;
                case colony::EventType::PawnAction:
                    r.dragged += e.action == colony::PawnAction::LayPawn && e.message == "safe" ? 1 : 0;
                    break;
                default: break;
            }
        }
        if (r.skipped) {
            break;
        }
        if (r.arrived && !r.wiped && r.colonistsDowned + r.colonistsDied >= 6 && sim.time().seconds % 60 == 0) {
            bool anyUp = false;
            for (const auto [pawn, colonist] : sim.things().registry().view<const colony::Colonist>().each()) {
                anyUp = anyUp || (sim.fighters().factionOf(pawn) == colony::kPlayerFaction && !sim.health().incapacitated(pawn));
            }
            if (!anyUp) {
                r.wiped = true;
                r.length = sim.time().seconds - arrivedAt;
                break;
            }
        }
        if (r.arrived && sim.raids().state().squads.empty()) {
            r.ended = true;
            r.length = sim.time().seconds - arrivedAt;
        }
    }
    return r;
}

}  // namespace

int run(uint64_t firstSeed, int seeds, std::shared_ptr<const colony::DefDatabase> defs, bool verbose) {
    if (!defs || seeds < 1) {
        std::fprintf(stderr, "--raid-soak needs the game's defs and at least one seed\n");
        return 2;
    }
    Tally fights;
    StuckWatch fightWatch;
    int raidsEnded = 0;
    int raidsMissed = 0;
    int raidsOpen = 0;
    int raidsWiped = 0;
    int raidsSkipped = 0;
    int64_t raidLength = 0;
    int64_t longest = 0;
    int colonistsDowned = 0;
    int colonistsDied = 0;
    int raidersDowned = 0;
    int raidersDied = 0;
    StuckWatch raidWatch;
    // The same raid with a Medic and a Firefighter (F10).
    int careEnded = 0;
    int careOpen = 0;
    int careMissed = 0;
    int careDied = 0;
    int careTended = 0;
    int careDragged = 0;
    StuckWatch careWatch;  // reported, not counted in the stuck rule
    std::printf("seed  fight: steps downed died waiting open%%   raid: ended length(min) colonists down/dead  raiders down/dead"
                "   with medics: ended died tended dragged\n");
    for (int k = 0; k < seeds; ++k) {
        const uint64_t seed = firstSeed + static_cast<uint64_t>(k);
        Tally t;
        firefight(seed, defs, t, fightWatch);
        const RaidResult r = raid(seed, defs, raidWatch, false);
        const RaidResult c = raid(seed, defs, careWatch, true);
        careEnded += c.ended ? 1 : 0;
        careOpen += c.arrived && !c.ended && !c.wiped ? 1 : 0;
        careMissed += c.arrived || c.skipped ? 0 : 1;
        careDied += c.colonistsDied;
        careTended += c.tendedInFight;
        careDragged += c.dragged;
        fights.waiting += t.waiting;
        fights.inOpen += t.inOpen;
        fights.downed += t.downed;
        fights.died += t.died;
        fights.fightSteps += t.fightSteps;
        raidsSkipped += r.skipped ? 1 : 0;
        raidsEnded += r.ended ? 1 : 0;
        raidsMissed += r.arrived || r.skipped ? 0 : 1;
        raidsOpen += r.arrived && !r.ended && !r.wiped ? 1 : 0;
        raidsWiped += r.wiped ? 1 : 0;
        raidLength += r.length;
        longest = std::max(longest, r.length);
        colonistsDowned += r.colonistsDowned;
        colonistsDied += r.colonistsDied;
        raidersDowned += r.raidersDowned;
        raidersDied += r.raidersDied;
        std::printf("%4llu  %11lld %6d %4d %7lld %5.1f   %17s %11lld %9d/%d %14d/%d   %17s %4d %6d %7d\n", static_cast<unsigned long long>(seed),
                    static_cast<long long>(t.fightSteps), t.downed, t.died, static_cast<long long>(t.waiting),
                    t.waiting > 0 ? 100.0 * static_cast<double>(t.inOpen) / static_cast<double>(t.waiting) : 0.0,
                    r.skipped ? "no start room" : r.arrived ? (r.ended ? "yes" : r.wiped ? "colony down" : "NO (still on)") : "NO (never came)",
                    static_cast<long long>(r.length / 60), r.colonistsDowned, r.colonistsDied, r.raidersDowned, r.raidersDied,
                    c.skipped ? "no start room" : c.arrived ? (c.ended ? "yes" : c.wiped ? "colony down" : "NO (still on)") : "NO (never came)",
                    c.colonistsDied, c.tendedInFight, c.dragged);
        if (verbose) {
            std::fflush(stdout);
        }
    }
    const double openPercent = fights.waiting > 0 ? 100.0 * static_cast<double>(fights.inOpen) / static_cast<double>(fights.waiting) : 0.0;
    const int stuck = fightWatch.stuck() + raidWatch.stuck();
    std::printf("{\"raidSoak\":{\"seeds\":%d,\"firstSeed\":%llu,\"waitingPawnSteps\":%lld,\"inOpenPercent\":%.2f,"
                "\"fightMeanSteps\":%.0f,\"fightDowned\":%d,\"fightDied\":%d,\"raidsEnded\":%d,\"raidsColonyDown\":%d,\"raidsStillOn\":%d,"
                "\"raidsNoStartRoom\":%d,\"raidsNeverCame\":%d,\"raidMeanMinutes\":%.1f,\"raidLongestMinutes\":%lld,\"colonistsDowned\":%d,"
                "\"colonistsDied\":%d,\"raidersDowned\":%d,\"raidersDied\":%d,\"stuckFighters\":%d,\"stuckInFights\":%d,\"stuckInRaids\":%d}}\n",
                seeds, static_cast<unsigned long long>(firstSeed), static_cast<long long>(fights.waiting), openPercent,
                static_cast<double>(fights.fightSteps) / seeds, fights.downed, fights.died, raidsEnded, raidsWiped, raidsOpen,
                raidsSkipped, raidsMissed, raidsEnded > 0 ? static_cast<double>(raidLength) / raidsEnded / 60.0 : 0.0,
                static_cast<long long>(longest / 60), colonistsDowned, colonistsDied, raidersDowned, raidersDied, stuck, fightWatch.stuck(), raidWatch.stuck());
    std::printf("{\"raidSoakMedics\":{\"raidsEnded\":%d,\"raidsStillOn\":%d,\"raidsNeverCame\":%d,\"colonistsDied\":%d,"
                "\"tendedInFight\":%d,\"dragged\":%d,\"stuckFighters\":%d}}\n",
                careEnded, careOpen, careMissed, careDied, careTended, careDragged, careWatch.stuck());
    bool ok = true;
    if (fights.waiting == 0 || openPercent >= kOpenLimit) {
        std::fprintf(stderr, "check failed: fighters waited in the open %.1f%% of the time (rule: under %.0f%%, %lld pawn-steps)\n",
                     openPercent, kOpenLimit, static_cast<long long>(fights.waiting));
        ok = false;
    }
    // Fighters that walk but get nowhere for kStuckSteps.
    if (stuck * kOneIn > seeds) {
        std::fprintf(stderr, "check failed: %d fighters stuck (walking but getting nowhere for %d steps; at most one seed in %d)\n", stuck,
                     kStuckSteps, kOneIn);
        ok = false;
    } else if (stuck > 0) {
        std::fprintf(stderr, "note: %d fighters stuck (walking but getting nowhere for %d steps)\n", stuck, kStuckSteps);
    }
    // With two colonists out of the firing line the colony is weaker and more raids hold on (field-care.md): only a raid
    // that never came fails; the rest are numbers to watch.
    if (careMissed > 0) {
        std::fprintf(stderr, "check failed: with medics, %d raids never came\n", careMissed);
        ok = false;
    } else if (careOpen > 0) {
        std::fprintf(stderr, "note: with medics, %d raids were still going after a quarter day\n", careOpen);
    }
    if (raidsMissed > 0 || raidsOpen * kOneIn > seeds) {
        std::fprintf(stderr, "check failed: %d raids never came, %d never ended inside the quarter day (at most one seed in %d)\n", raidsMissed,
                     raidsOpen, kOneIn);
        ok = false;
    } else if (raidsOpen > 0) {
        std::fprintf(stderr, "note: %d raids were still going after a quarter day\n", raidsOpen);
    }
    return ok ? 0 : 1;
}

}  // namespace raidsoak

// Team play without fixed squads (docs/design/team-play.md).
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

namespace {

// The game's real data, with team play on (as shipped) or switched off in FighterRules.
std::shared_ptr<const DefDatabase> teamTestDefs(bool teamPlay) {
    static std::shared_ptr<const DefDatabase> cached[2];
    std::shared_ptr<const DefDatabase>& slot = cached[teamPlay ? 1 : 0];
    if (slot != nullptr) {
        return slot;
    }
    namespace fs = std::filesystem;
    const fs::path root = fs::path(COLONY_DATA_DIR);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    std::vector<DefSource> sources;
    for (const fs::path& file : files) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::string body = text.str();
        if (!teamPlay && file.filename() == "FighterRules.json") {
            const std::string on = "\"teamPlay\": true";
            const size_t at = body.find(on);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                body.replace(at, on.size(), "\"teamPlay\": false");
            }
        }
        sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), body});
    }
    std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
    std::stringstream tuning;
    tuning << tuningFile.rdbuf();
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, tuning.str(), *db, errors);
    for (const std::string& e : errors) {
        MESSAGE(e);
    }
    slot = db;
    return slot;
}

std::unique_ptr<Sim> teamTestSim(bool teamPlay = true, uint64_t seed = 5) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, teamTestDefs(teamPlay));
}

Entity teamTestPawn(Sim& sim, int x, int y, const char* gun) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    if (gun != nullptr) {
        sim.combat().equip(pawn, sim.things().make(sim.defs().idOf("Things", gun)));
    }
    return pawn;
}

// Keeps a pawn from bleeding out or going down, so the test watches the choices and not who wins.
void teamTestHeal(Sim& sim, Entity pawn) {
    sim.things().registry().replace<Health>(pawn, Health{});
    sim.health().refresh(pawn);
}

struct TeamFight {
    std::unique_ptr<Sim> sim;
    std::vector<Entity> colonists;
    Entity raider = kNoEntity;
    Entity raider2 = kNoEntity;
};

// Three riflemen against two raiders with rifles 24 m off behind a line of sandbags facing them.
TeamFight teamTestFight(bool teamPlay, uint64_t seed = 5) {
    TeamFight t;
    t.sim = teamTestSim(teamPlay, seed);
    for (int i = 0; i < 3; ++i) {
        t.colonists.push_back(teamTestPawn(*t.sim, 40 + 3 * i, 36, "Gun_AssaultRifle"));
        t.sim->submit(DraftCommand{t.colonists.back(), true});
    }
    for (int x = 39; x <= 48; ++x) {
        t.sim->things().spawn("Sandbags", CellPos{x, 59, 0});
    }
    t.raider = teamTestPawn(*t.sim, 42, 60, "Gun_AssaultRifle");
    t.raider2 = teamTestPawn(*t.sim, 45, 60, "Gun_AssaultRifle");
    t.sim->submit(SetFactionCommand{t.raider, 2});
    t.sim->submit(SetFactionCommand{t.raider2, 2});
    t.sim->step();
    for (const Entity c : t.colonists) {
        t.sim->things().registry().get<Fighter>(c).leash = 0.0f;  // called up by the alarm, not held by a draft
    }
    return t;
}

int teamTestCallouts(const std::vector<Event>& events, const char* words, std::set<std::string>* seen = nullptr) {
    int n = 0;
    for (const Event& e : events) {
        if (e.type == EventType::PawnAction && e.action == PawnAction::Callout) {
            if (seen != nullptr) {
                seen->insert(e.message);
            }
            if (words == nullptr || e.message.rfind(words, 0) == 0) {
                ++n;
            }
        }
    }
    return n;
}

}  // namespace

TEST_CASE("Team play: each fighter's boldness is its own, the same every time, raiders bolder than colonists") {
    auto sim = teamTestSim();
    std::vector<Entity> pawns;
    for (int i = 0; i < 12; ++i) {
        pawns.push_back(teamTestPawn(*sim, 20 + 2 * i, 20, nullptr));
    }
    for (int i = 6; i < 12; ++i) {
        sim->submit(SetFactionCommand{pawns[static_cast<size_t>(i)], 2});
    }
    sim->step();
    std::set<int> distinct;
    float colonists = 0.0f;
    float raiders = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const float a = sim->fighters().aggression(pawns[static_cast<size_t>(i)]);
        CHECK(a >= 0.0f);
        CHECK(a <= 1.0f);
        CHECK(a == sim->fighters().aggression(pawns[static_cast<size_t>(i)]));
        distinct.insert(static_cast<int>(std::lround(a * 1000.0f)));
        (i < 6 ? colonists : raiders) += a / 6.0f;
    }
    MESSAGE("colonists " << colonists << ", raiders " << raiders);
    CHECK(distinct.size() >= 8);  // not all alike
    CHECK(raiders > colonists);
}

TEST_CASE("Team play: one fighter lays down fire, the others go round the side and call it out") {
    TeamFight t = teamTestFight(true);
    Sim& sim = *t.sim;
    std::set<std::string> said;
    int flanking = 0;
    int suppressing = 0;
    float widest = 0.0f;  // degrees between a flanker and the friends shooting from the front, seen from the raider
    for (int i = 0; i < 600; ++i) {
        for (const Entity c : t.colonists) {
            teamTestHeal(sim, c);
        }
        teamTestHeal(sim, t.raider);
        teamTestHeal(sim, t.raider2);
        sim.step();
        teamTestCallouts(sim.events(), nullptr, &said);
        flanking += teamTestCallouts(sim.events(), "flanking");
        suppressing += teamTestCallouts(sim.events(), "suppressing");
        const entt::registry& reg = sim.things().registry();
        const Position& r = reg.get<Position>(t.raider);
        for (const Entity c : t.colonists) {
            const Fighter& f = reg.get<Fighter>(c);
            if (f.role != TeamRole::Flank) {
                continue;
            }
            const Position& me = reg.get<Position>(c);
            for (const Entity o : t.colonists) {
                const Fighter& other = reg.get<Fighter>(o);
                if (o == c || other.role == TeamRole::Flank || !other.canFire) {
                    continue;
                }
                const Position& p = reg.get<Position>(o);
                const float a1 = std::atan2(me.y - r.y, me.x - r.x);
                const float a2 = std::atan2(p.y - r.y, p.x - r.x);
                float d = std::fabs(a1 - a2) * 57.29578f;
                d = std::min(d, 360.0f - d);
                widest = std::max(widest, d);
            }
        }
    }
    std::string list;
    for (const std::string& s : said) {
        list += s + "; ";
    }
    MESSAGE("calls: " << list << " flank roles " << sim.fighters().roleCount(TeamRole::Flank) << ", suppress roles "
                      << sim.fighters().roleCount(TeamRole::Suppress) << ", widest flank " << widest << " degrees");
    CHECK(sim.fighters().roleCount(TeamRole::Suppress) > 0);
    CHECK(sim.fighters().roleCount(TeamRole::Flank) > 0);
    CHECK(suppressing + static_cast<int>(sim.fighters().roleCount(TeamRole::Suppress)) > 0);
    CHECK(flanking > 0);
    CHECK(widest > 25.0f);  // the flanker really works round to the side
}

TEST_CASE("Team play: switched off in FighterRules, nobody takes a part or calls out") {
    TeamFight t = teamTestFight(false);
    Sim& sim = *t.sim;
    int calls = 0;
    for (int i = 0; i < 600; ++i) {
        for (const Entity c : t.colonists) {
            teamTestHeal(sim, c);
        }
        teamTestHeal(sim, t.raider);
        teamTestHeal(sim, t.raider2);
        sim.step();
        calls += teamTestCallouts(sim.events(), nullptr);
        for (const Entity c : t.colonists) {
            CHECK(sim.things().registry().get<Fighter>(c).role == TeamRole::None);
            CHECK(sim.things().registry().get<Fighter>(c).suppressedUntil == 0);
        }
    }
    CHECK(calls == 0);
    for (int r = 0; r <= static_cast<int>(TeamRole::FallBack); ++r) {
        CHECK(sim.fighters().roleCount(static_cast<TeamRole>(r)) == 0);
    }
}

TEST_CASE("Team play: a friend going down near a timid fighter is called out and shakes it into falling back") {
    auto sim = teamTestSim();
    const Entity a = teamTestPawn(*sim, 40, 36, "Gun_Revolver");
    const Entity b = teamTestPawn(*sim, 43, 36, "Gun_Revolver");
    const Entity raider = teamTestPawn(*sim, 41, 56, "Gun_Revolver");
    const Entity raider2 = teamTestPawn(*sim, 44, 56, "Gun_Revolver");  // two against two: nobody outnumbered
    sim->submit(DraftCommand{a, true});
    sim->submit(DraftCommand{b, true});
    sim->submit(SetFactionCommand{raider, 2});
    sim->submit(SetFactionCommand{raider2, 2});
    sim->step();
    for (int i = 0; i < 60; ++i) {
        for (const Entity e : {a, b, raider, raider2}) {
            teamTestHeal(*sim, e);
        }
        sim->step();
    }
    // Down `b` the way a bullet would: the sim's own event, then let `a` think.
    entt::registry& reg = sim->things().registry();
    Fighter& fa = reg.get<Fighter>(a);
    std::vector<Event> lost(1);
    lost[0].type = EventType::PawnDowned;
    lost[0].thing = b;
    sim->fighters().noteLosses(lost, 0);
    CHECK(fa.alliesDown == 1);
    lost[0].thing = raider;  // an enemy's loss doesn't count for it
    sim->fighters().noteLosses(lost, 0);
    CHECK(fa.alliesDown == 1);
    // Three friends lost in a short while shake even a bold fighter.
    lost[0].thing = b;
    sim->fighters().noteLosses(lost, 0);
    sim->fighters().noteLosses(lost, 0);
    sim->fighters().noteLosses(lost, 0);
    CHECK(fa.alliesDown == 4);
    bool fellBack = false;
    for (int i = 0; i < 40 && !fellBack; ++i) {
        for (const Entity e : {a, raider, raider2}) {
            teamTestHeal(*sim, e);
        }
        sim->things().registry().emplace_or_replace<Stunned>(b, Stunned{5});
        sim->step();
        fellBack = reg.get<Fighter>(a).fallingBack && reg.get<Fighter>(a).role == TeamRole::FallBack;
    }
    CHECK(fellBack);
}

TEST_CASE("Team play: a fight is the same after a save and load") {
    TeamFight straight = teamTestFight(true, 9);
    TeamFight saved = teamTestFight(true, 9);
    for (int i = 0; i < 900; ++i) {
        straight.sim->step();
        saved.sim->step();
        if (i == 300) {
            SaveLoadResult loaded = loadSim(saveSim(*saved.sim), teamTestDefs(true));
            CHECK(loaded.sim != nullptr);
            if (loaded.sim == nullptr) {
                return;
            }
            CHECK(loaded.sim->stateHash() == saved.sim->stateHash());
            saved.sim = std::move(loaded.sim);
        }
    }
    CHECK(saved.sim->stateHash() == straight.sim->stateHash());
}


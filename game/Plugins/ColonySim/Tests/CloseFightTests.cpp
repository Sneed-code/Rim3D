// Close-range firefights after a wall breach (C25, docs/design/colony-scenario.md "Fifth pass").
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Animals.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> closeFightDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        namespace fs = std::filesystem;
        const fs::path root = fs::path(COLONY_DATA_DIR);
        std::vector<DefSource> sources;
        std::vector<fs::path> files;
        for (const auto& entry : fs::recursive_directory_iterator(root / "defs")) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            sources.push_back(DefSource{file.parent_path().filename().string(), file.filename().string(), text.str()});
        }
        std::ifstream tuningFile(root / "tuning.json", std::ios::binary);
        std::stringstream tuning;
        tuning << tuningFile.rdbuf();
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::build(sources, tuning.str(), *db, errors);
        return db;
    }();
    return defs;
}

struct CloseFightResult {
    int colonistShots = 0;
    int raiderShots = 0;
    int raiderHits = 0;    // injuries the raider took
    int colonistHits = 0;  // injuries the colonists took
    int swings = 0;
    bool raiderDown = false;
    int colonistsDown = 0;
    int steps = 0;
};

// A raider with `raiderGun` steps through a gap in a wall into a room with three defending colonists `gap` m away.
CloseFightResult closeFightRun(uint64_t seed, const char* raiderGun, const char* colonistGun, int gap) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    auto sim = std::make_unique<Sim>(settings, closeFightDefs());
    // Room 30..40 x 30..40, walls on the edges, a gap at (35, 30).
    for (int i = 30; i <= 40; ++i) {
        for (const CellPos c : {CellPos{i, 30, 0}, CellPos{i, 40, 0}, CellPos{30, i, 0}, CellPos{40, i, 0}}) {
            if (!(c.x == 35 && c.y == 30)) {
                sim->things().spawn("Wall", c);
            }
        }
    }
    const Entity raider = sim->things().spawn("Human", CellPos{35, 31, 0});
    sim->combat().equip(raider, sim->things().make(sim->defs().idOf("Things", raiderGun)));
    // Its squad, out of sight behind a wall outside and held still, so it doesn't fall back as one against three.
    for (int x = 20; x <= 26; ++x) {
        sim->things().spawn("Wall", CellPos{x, 26, 0});
    }
    std::vector<Entity> squad;
    for (const int x : {22, 24}) {
        const Entity e = sim->things().spawn("Human", CellPos{x, 24, 0});
        sim->combat().equip(e, sim->things().make(sim->defs().idOf("Things", raiderGun)));
        sim->submit(SetFactionCommand{e, 2});
        squad.push_back(e);
    }
    std::vector<Entity> colonists;
    for (const int dx : {-1, 0, 1}) {
        const Entity c = sim->things().spawn("Human", CellPos{35 + dx, 31 + gap, 0});
        if (colonistGun != nullptr) {
            sim->combat().equip(c, sim->things().make(sim->defs().idOf("Things", colonistGun)));
        }
        sim->submit(DraftCommand{c, true});
        colonists.push_back(c);
    }
    sim->submit(SetFactionCommand{raider, 2});
    sim->step();
    for (const Entity c : colonists) {
        sim->things().registry().get<Fighter>(c).defending = true;
    }
    CloseFightResult r;
    for (int i = 0; i < 1800; ++i) {
        for (const Entity e : squad) {
            sim->things().registry().emplace_or_replace<Stunned>(e, Stunned{5});
        }
        sim->step();
        r.steps = i;
        for (const Event& e : sim->events()) {
            if (e.type == EventType::ShotFired) {
                (e.thing == raider ? r.raiderShots : r.colonistShots) += 1;
            } else if (e.type == EventType::PawnInjured) {
                (e.thing == raider ? r.raiderHits : r.colonistHits) += 1;
            } else if (e.type == EventType::MeleeSwing && e.thing != raider) {
                r.swings += 1;
            }
        }
        r.raiderDown = sim->health().incapacitated(raider) || sim->things().registry().get<Health>(raider).dead;
        r.colonistsDown = 0;
        for (const Entity c : colonists) {
            if (sim->health().incapacitated(c) || sim->things().registry().get<Health>(c).dead) {
                ++r.colonistsDown;
            }
        }
        if (r.raiderDown || r.colonistsDown == 3) {
            break;
        }
    }
    return r;
}


struct InsectFightResult {
    int clubSwings = 0;
    int friendlyGunshots = 0;
    int shots = 0;
    int insectHits = 0;
    bool insectOut = false;
    int seconds = 0;
};

// Seed 42 of the year run, day 36: a chitinid got among the colonists; one with a club stood beside it, one with a
// revolver 1.7 m away shot at it and hit the clubman, and the clubman never swung.
InsectFightResult insectFightRun(uint64_t seed, const char* insectDef) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.checkpointEvery = 0;
    auto sim = std::make_unique<Sim>(settings, closeFightDefs());
    for (int y = 0; y < sim->map().height(); ++y) {
        for (int x = 0; x < sim->map().width(); ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    const auto place = [&](const char* def, float x, float y, const char* weapon) {
        const Entity e = sim->things().spawn(def, CellPos{static_cast<int>(x), static_cast<int>(y), 0});
        Position& p = sim->things().registry().get<Position>(e);
        p.x = x;
        p.y = y;
        if (weapon != nullptr) {
            sim->combat().equip(e, sim->things().make(sim->defs().idOf("Things", weapon)));
        }
        return e;
    };
    const Entity insect = place(insectDef, 30.5f, 30.5f, nullptr);
    const Entity clubman = place("Human", 31.4f, 30.5f, "Melee_Club");
    const Entity gunman = place("Human", 33.0f, 30.8f, "Gun_Revolver");
    (void)sim->things().registry().get_or_emplace<Colonist>(clubman);
    (void)sim->things().registry().get_or_emplace<Colonist>(gunman);
    sim->animals().enrage(insect, AnimalTemper::Manhunter, clubman);
    InsectFightResult r;
    const Health& h = sim->things().registry().get<Health>(insect);
    for (int i = 0; i < 900 && !h.dead && !h.downed; ++i) {
        sim->step();
        r.seconds = i;
        for (const Event& e : sim->events()) {
            r.clubSwings += e.type == EventType::MeleeSwing && e.thing == clubman ? 1 : 0;
            r.shots += e.type == EventType::ShotFired ? 1 : 0;
            r.insectHits += e.type == EventType::PawnInjured && e.thing == insect ? 1 : 0;
            r.friendlyGunshots += e.type == EventType::PawnInjured && (e.thing == clubman || e.thing == gunman) &&
                                          e.def.valid() && sim->defs().get(e.def).defName() == "Gunshot"
                                      ? 1
                                      : 0;
        }
    }
    r.insectOut = h.dead || h.downed;
    return r;
}

struct LineFightResult {
    int shots = 0;
    int friendlyGunshots = 0;
    int raiderGunshots = 0;
    bool raiderOut = false;
};

// A raider with a knife fights a colonist with a club hand to hand; a colonist with a revolver stands behind the
// clubman, in line. `corridor`: all three in a corridor one cell wide, so there is no stepping aside.
LineFightResult lineFightRun(uint64_t seed, bool corridor) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 64;
    settings.flatMap = true;
    settings.checkpointEvery = 0;
    auto sim = std::make_unique<Sim>(settings, closeFightDefs());
    for (int y = 0; y < sim->map().height(); ++y) {
        for (int x = 0; x < sim->map().width(); ++x) {
            sim->plants().remove(CellPos{x, y, 0});
        }
    }
    if (corridor) {
        for (int x = 20; x <= 40; ++x) {
            sim->things().spawn("Wall", CellPos{x, 29, 0});
            sim->things().spawn("Wall", CellPos{x, 31, 0});
        }
        sim->things().spawn("Wall", CellPos{20, 30, 0});
        sim->things().spawn("Wall", CellPos{40, 30, 0});
    }
    const auto place = [&](float x, float y, const char* weapon) {
        const Entity e = sim->things().spawn("Human", CellPos{static_cast<int>(x), static_cast<int>(y), 0});
        Position& p = sim->things().registry().get<Position>(e);
        p.x = x;
        p.y = y;
        sim->combat().equip(e, sim->things().make(sim->defs().idOf("Things", weapon)));
        return e;
    };
    const Entity raider = place(27.5f, 30.5f, "Melee_Knife");
    const Entity clubman = place(28.4f, 30.5f, "Melee_Club");
    const Entity gunman = place(31.5f, 30.5f, "Gun_Revolver");
    sim->submit(DraftCommand{clubman, true});
    sim->submit(DraftCommand{gunman, true});
    sim->submit(SetFactionCommand{raider, 2});
    LineFightResult r;
    const Health& h = sim->things().registry().get<Health>(raider);
    for (int i = 0; i < 600 && !h.dead && !h.downed; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            r.shots += e.type == EventType::ShotFired && e.thing == gunman ? 1 : 0;
            const bool gunshot = e.type == EventType::PawnInjured && e.def.valid() && sim->defs().get(e.def).defName() == "Gunshot";
            r.friendlyGunshots += gunshot && e.thing == clubman ? 1 : 0;
            r.raiderGunshots += gunshot && e.thing == raider ? 1 : 0;
        }
    }
    r.raiderOut = h.dead || h.downed;
    return r;
}
}  // namespace

TEST_CASE("Close fights: a gunman behind a friend fighting hand to hand steps aside to shoot, and never hits the friend") {
    // Seed 42 of the year run, day 36: a colonist with a revolver shot at a chitinid 2.6 m away through the colonist with a
    // club fighting it, again and again, and downed it. Before the fix the gunman hit its friend 2 to 4 times a fight.
    for (uint64_t seed = 1; seed <= 3; ++seed) {
        const LineFightResult r = lineFightRun(seed, false);
        MESSAGE("seed " << seed << ": shots " << r.shots << ", on the raider " << r.raiderGunshots << ", on the friend " << r.friendlyGunshots);
        CHECK(r.friendlyGunshots == 0);
        CHECK(r.raiderGunshots > 0);
        CHECK(r.raiderOut);
    }
}

TEST_CASE("Close fights: in a corridor with no stepping aside the gunman holds its fire rather than shoot its friend") {
    for (uint64_t seed = 1; seed <= 3; ++seed) {
        const LineFightResult r = lineFightRun(seed, true);
        MESSAGE("seed " << seed << ": shots " << r.shots << ", on the raider " << r.raiderGunshots << ", on the friend " << r.friendlyGunshots);
        CHECK(r.friendlyGunshots == 0);
    }
}

TEST_CASE("Close fights: a gunman next to a friend fighting a chitinid shoots it without hitting the friend") {
    for (const char* insect : {"Megascarab", "Spelopede", "Megaspider"}) {
        const InsectFightResult r = insectFightRun(1, insect);
        MESSAGE(std::string(insect) << ": " << r.seconds << " s, club swings " << r.clubSwings << ", shots " << r.shots << ", on the friends "
                       << r.friendlyGunshots);
        CHECK(r.insectOut);
        CHECK(r.clubSwings > 0);
        CHECK(r.friendlyGunshots == 0);
    }
}

TEST_CASE("Close fights: three colonists beat a raider who comes through a gap in the wall 2 to 4 m away") {
    for (const char* gun : {"Gun_PumpShotgun", "Gun_Revolver", "Melee_Mace"}) {
        for (const int gap : {2, 4}) {
            int wins = 0;
            for (uint64_t seed = 1; seed <= 3; ++seed) {
                const CloseFightResult r = closeFightRun(seed, gun, "Gun_Revolver", gap);
                wins += r.raiderDown && r.colonistsDown < 3 ? 1 : 0;
                CHECK(r.colonistsDown <= 1);
            }
            MESSAGE(std::string(gun) << " at " << gap << " m: colonists won " << wins << " of 3");
            CHECK(wins == 3);
        }
    }
}

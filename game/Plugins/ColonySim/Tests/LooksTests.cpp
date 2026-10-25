#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include "TestDefs.h"

#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

namespace {

std::shared_ptr<const DefDatabase> looksTestDefs() {
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    CHECK(ok);
    return db;
}

bool looksSameColor(const LooksColor& a, const LooksColor& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool looksSame(const PawnLooks& a, const PawnLooks& b) {
    return a.lineage == b.lineage && a.female == b.female && a.body == b.body && a.weight == b.weight &&
           a.height == b.height && a.melanin == b.melanin && looksSameColor(a.naturalHair, b.naturalHair) &&
           looksSameColor(a.skin, b.skin) && looksSameColor(a.hair, b.hair);
}

float looksRedness(const LooksColor& c) {
    return c.r - (c.g + c.b) * 0.5f;
}

SimSettings looksCrashlanded() {
    SimSettings settings;
    settings.seed = 11;
    settings.mapSize = 96;
    settings.flatMap = true;
    settings.scenario = "Crashlanded";
    return settings;
}

}  // namespace

TEST_CASE("looks: the same seed gives the same looks, other seeds give others") {
    const auto defs = looksTestDefs();
    PawnLooks a, b, c;
    std::string error;
    CHECK(generateLooks(*defs, 5, false, "Baseline", a, error));
    CHECK(generateLooks(*defs, 5, false, "Baseline", b, error));
    CHECK(generateLooks(*defs, 6, false, "Baseline", c, error));
    CHECK(error.empty());
    CHECK(looksSame(a, b));
    CHECK_FALSE(looksSame(a, c));
}

TEST_CASE("looks: bodies, heights and colours follow the rules and vary") {
    const auto defs = looksTestDefs();
    std::map<BodyType, int> bodies;
    float lightest = 1.0f;
    float darkest = 0.0f;
    float tallestWoman = 0.0f;
    float shortestMan = 9.0f;
    std::map<float, int> hairs;
    for (uint64_t seed = 1; seed <= 600; ++seed) {
        const bool female = seed % 2 == 0;
        PawnLooks l;
        std::string error;
        CHECK(generateLooks(*defs, seed, female, "Baseline", l, error));
        CHECK(l.female == female);
        CHECK(l.lineage == "Baseline");
        CHECK(l.weight >= -1.0f);
        CHECK(l.weight <= 1.0f);
        ++bodies[l.body];
        switch (l.body) {
            case BodyType::Thin: CHECK(l.weight <= -0.55f); break;
            case BodyType::Fat: CHECK(l.weight >= 0.55f); break;
            case BodyType::Average: CHECK(std::fabs(l.weight) <= 0.35f); break;
            case BodyType::Hulk: CHECK(l.weight >= 0.15f); break;
        }
        if (female) {
            tallestWoman = std::max(tallestWoman, l.height);
        } else {
            shortestMan = std::min(shortestMan, l.height);
        }
        CHECK(l.height > 0.85f);
        CHECK(l.height < 1.1f);
        CHECK(l.melanin >= 0.0f);
        CHECK(l.melanin <= 1.0f);
        // Baseline skin is the melanin colour: lighter for less melanin.
        const float brightness = l.skin.r + l.skin.g + l.skin.b;
        if (l.melanin < 0.1f) lightest = std::min(lightest, brightness / 3.0f);
        if (l.melanin > 0.9f) darkest = std::max(darkest, brightness / 3.0f);
        CHECK(looksSameColor(l.hair, l.naturalHair));
        ++hairs[l.naturalHair.r];
    }
    CHECK(bodies.size() == 4);
    CHECK(bodies[BodyType::Average] > bodies[BodyType::Thin]);
    CHECK(bodies[BodyType::Thin] > bodies[BodyType::Fat]);
    CHECK(lightest > darkest + 0.25f);
    // Women are shorter on the whole; the ranges overlap only a little (a Hulk is a bit taller).
    CHECK(tallestWoman < 1.02f);
    CHECK(shortestMan > 0.94f);
    CHECK(hairs.size() == 6);
}

TEST_CASE("looks: an Emberkin has red skin and the lineage's hair; changing lineage keeps the body") {
    const auto defs = looksTestDefs();
    std::string error;
    PawnLooks baseline, ember;
    CHECK(generateLooks(*defs, 42, true, "Baseline", baseline, error));
    CHECK(generateLooks(*defs, 42, true, "Emberkin", ember, error));
    CHECK(error.empty());
    CHECK(ember.body == baseline.body);
    CHECK(ember.weight == baseline.weight);
    CHECK(ember.melanin == baseline.melanin);
    CHECK(looksRedness(ember.skin) > 0.2f);
    CHECK(looksRedness(ember.skin) > looksRedness(baseline.skin) + 0.15f);
    CHECK(ember.hair.r < 0.15f);

    PawnLooks changed = baseline;
    CHECK(setLineage(*defs, "Emberkin", changed, error));
    CHECK(looksSame(changed, ember));
    CHECK(setLineage(*defs, "Baseline", changed, error));
    CHECK(looksSame(changed, baseline));
    CHECK_FALSE(setLineage(*defs, "NoSuchLineage", changed, error));
    CHECK(error.find("NoSuchLineage") != std::string::npos);
    CHECK(looksSame(changed, baseline));

    PawnLooks unknown;
    CHECK_FALSE(generateLooks(*defs, 42, true, "NoSuchLineage", unknown, error));
}

TEST_CASE("looks: without the looks rules a pawn gets plain looks") {
    const auto defs = testDefs();
    PawnLooks l;
    std::string error;
    CHECK_FALSE(generateLooks(*defs, 3, false, "Baseline", l, error));
    CHECK(error.find("LooksRules") != std::string::npos);
    CHECK(l.body == BodyType::Average);
    CHECK(l.height == 1.0f);
}

TEST_CASE("looks: colonists, haulers and spawned pawns get looks; items don't; the snapshot shows them") {
    const auto defs = looksTestDefs();
    Sim sim(looksCrashlanded(), defs);
    CHECK(sim.setupErrors().empty());
    int colonists = 0;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        const PawnLooks* looks = sim.looksOf(pawn);
        if (looks == nullptr) {
            CHECK(looks != nullptr);
            continue;
        }
        CHECK(looks->female == colonist.female);
        CHECK(looks->lineage == "Baseline");
        ++colonists;
    }
    CHECK(colonists == 6);

    sim.submit(SpawnThingCommand{"Human", CellPos{10, 10, 0}});
    sim.step();
    Snapshot snapshot;
    sim.writeSnapshot(snapshot);
    int pawns = 0;
    for (const ThingView& view : snapshot.things) {
        if (view.category == ThingCategory::Pawn) {
            ++pawns;
            if (view.looks < 0 || view.looks >= static_cast<int32_t>(snapshot.looks.size())) {
                CHECK(view.looks >= 0);
                continue;
            }
            const PawnLooksView& l = snapshot.looks[static_cast<size_t>(view.looks)];
            const PawnLooks* looks = sim.looksOf(view.id);
            CHECK(looks != nullptr);
            CHECK(l.lineage == defs->idOf("Lineages", "Baseline"));
            CHECK(l.height == looks->height);
            CHECK(looksSameColor(l.skin, looks->skin));
        } else {
            CHECK(view.looks == -1);
        }
    }
    CHECK(pawns == 7);
    CHECK(snapshot.looks.size() == 7);

    // The haulers of the hauling test (test defs: no rules, plain looks) have looks too.
    Sim hauling(SimSettings{3, 64, 4, 10, true}, testDefs());
    for (const Entity hauler : hauling.haulers()) {
        CHECK(hauling.looksOf(hauler) != nullptr);
    }
}

TEST_CASE("looks: the SetLineage command recolours a pawn, and saves and replays keep it") {
    const auto defs = looksTestDefs();
    Sim sim(looksCrashlanded(), defs);
    const Entity pawn = *sim.things().registry().view<const Colonist>().begin();
    const PawnLooks before = *sim.looksOf(pawn);
    const uint64_t good = sim.submit(SetLineageCommand{pawn, "Emberkin"});
    const uint64_t badLineage = sim.submit(SetLineageCommand{pawn, "NoSuchLineage"});
    const uint64_t badPawn = sim.submit(SetLineageCommand{kNoEntity, "Emberkin"});
    sim.step();
    std::map<uint64_t, std::string> failed;
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) failed[e.command] = e.message;
    }
    CHECK(failed.count(good) == 0);
    CHECK(failed.count(badLineage) == 1);
    CHECK(failed.count(badPawn) == 1);
    const PawnLooks after = *sim.looksOf(pawn);
    CHECK(after.lineage == "Emberkin");
    CHECK(after.weight == before.weight);
    CHECK(looksRedness(after.skin) > 0.2f);

    // Save and load: the same looks, hash and file.
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    CHECK(loaded.error.empty());
    if (loaded.sim == nullptr) {
        CHECK(loaded.sim != nullptr);
        return;
    }
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const PawnLooks* reloaded = loaded.sim->looksOf(pawn);
    CHECK(reloaded != nullptr);
    if (reloaded != nullptr) {
        CHECK(looksSame(*reloaded, after));
    }

    // The command goes through a replay file and plays back the same.
    const std::string json = writeReplay(recordReplay(sim));
    std::string error;
    Replay replay;
    CHECK(readReplay(json, replay, error));
    bool found = false;
    for (const LoggedCommand& logged : replay.commands) {
        if (const auto* c = std::get_if<SetLineageCommand>(&logged.command); c && c->lineage == "Emberkin" && c->pawn == pawn) {
            found = true;
        }
    }
    CHECK(found);
}

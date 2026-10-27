#include "doctest.h"

#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Random.h"
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

TEST_CASE("looks: faces, hair styles, beards, body hair and tattoos follow the rules and vary") {
    const auto defs = looksTestDefs();
    std::map<std::string, int> hairStyles;
    std::map<std::string, int> beards;
    std::map<float, int> eyes;
    int bearded = 0;
    int men = 0;
    int tattooed = 0;
    int women = 0;
    float womanHair = 0.0f;
    float manHair = 0.0f;
    float faceLo = 1.0f;
    float faceHi = 0.0f;
    for (uint64_t seed = 1; seed <= 1000; ++seed) {
        const bool female = seed % 2 == 0;
        PawnLooks l, again;
        std::string error;
        CHECK(generateLooks(*defs, seed, female, "Baseline", l, error));
        CHECK(generateLooks(*defs, seed, female, "Baseline", again, error));
        CHECK(looksSame(l, again));
        CHECK(l.hairStyle == again.hairStyle);
        CHECK(l.faceShape == again.faceShape);
        CHECK(l.tattoos.size() == again.tattoos.size());
        for (const float v : {l.faceShape, l.brow, l.nose, l.jaw, l.bodyHair}) {
            CHECK(v >= 0.0f);
            CHECK(v <= 1.0f);
        }
        faceLo = std::min({faceLo, l.faceShape, l.nose});
        faceHi = std::max({faceHi, l.faceShape, l.nose});
        CHECK_FALSE(l.hairStyle.empty());
        CHECK(l.beard == l.naturalBeard);
        CHECK(l.bodyHair == l.naturalBodyHair);
        ++hairStyles[l.hairStyle];
        ++eyes[l.eyes.b];
        if (female) {
            CHECK(l.beard == "None");
            ++women;
            womanHair += l.bodyHair;
        } else {
            ++men;
            manHair += l.bodyHair;
            ++beards[l.beard];
            bearded += l.beard != "None";
        }
        CHECK(l.tattoos.size() <= 3);
        tattooed += !l.tattoos.empty();
        for (const LooksMark& m : l.tattoos) {
            CHECK(static_cast<int>(m.region) < kBodyRegionCount);
            CHECK(m.style < 10);
            CHECK(m.u >= 0.0f);
            CHECK(m.u <= 1.0f);
            CHECK(m.size >= 0.7f);
            CHECK(m.size <= 1.3f);
        }
        CHECK(l.scars.empty());  // scars come from injuries only
    }
    CHECK(hairStyles.size() >= 12);
    CHECK(beards.size() == 7);
    CHECK(eyes.size() >= 5);
    CHECK(bearded > men / 3);
    CHECK(bearded < men * 2 / 3);
    CHECK(tattooed > 1000 * 8 / 100);
    CHECK(tattooed < 1000 * 30 / 100);
    CHECK(manHair / static_cast<float>(men) > womanHair / static_cast<float>(women) + 0.3f);
    CHECK(faceLo < 0.2f);
    CHECK(faceHi > 0.8f);
}

TEST_CASE("looks: ten colonists side by side have ten different faces") {
    const auto defs = looksTestDefs();
    std::vector<PawnLooks> faces;
    for (uint64_t seed = 100; seed < 110; ++seed) {
        PawnLooks l;
        std::string error;
        CHECK(generateLooks(*defs, seed, seed % 2 == 0, "Baseline", l, error));
        faces.push_back(l);
    }
    for (size_t i = 0; i < faces.size(); ++i) {
        for (size_t j = i + 1; j < faces.size(); ++j) {
            CHECK((faces[i].faceShape != faces[j].faceShape || faces[i].nose != faces[j].nose));
        }
    }
}

TEST_CASE("looks: a beardless lineage has no beard, a furry one thick body hair, and changing back restores them") {
    const auto defs = looksTestDefs();
    std::string error;
    // A man with a beard in the Baseline lineage.
    PawnLooks man;
    for (uint64_t seed = 1; seed < 200; ++seed) {
        CHECK(generateLooks(*defs, seed, false, "Baseline", man, error));
        if (man.beard != "None") break;
    }
    CHECK(man.beard != "None");
    PawnLooks ember = man;
    CHECK(setLineage(*defs, "Emberkin", ember, error));
    CHECK(ember.beard == "None");
    CHECK(ember.naturalBeard == man.naturalBeard);
    CHECK(setLineage(*defs, "Baseline", ember, error));
    CHECK(ember.beard == man.beard);

    PawnLooks frost = man;
    CHECK(setLineage(*defs, "Frostkin", frost, error));
    CHECK(frost.bodyHair == doctest::Approx(1.0f));
    CHECK(frost.naturalBodyHair == man.naturalBodyHair);
    CHECK(frost.beard == man.beard);
    CHECK(setLineage(*defs, "Baseline", frost, error));
    CHECK(frost.bodyHair == man.bodyHair);
}

TEST_CASE("looks: a healed injury that leaves a scar marks the skin, and an organ's scar shows nothing") {
    const auto defs = looksTestDefs();
    Sim sim(SimSettings{7, 64, 0, 0, true}, defs);
    const DefId cut = defs->idOf("Hediffs", "Cut");
    const DefId scarDef = defs->idOf("Hediffs", "Scar");
    const DefId oldWound = defs->idOf("Hediffs", "OldWound");
    CHECK((cut.valid() && scarDef.valid() && oldWound.valid()));
    const Entity first = sim.things().spawn("Human", CellPos{2, 2, 0});
    const DefId human = sim.things().registry().get<Thing>(first).def;
    const std::vector<std::string> labels = sim.health().partLabels(human);
    const auto partIndex = [&](const char* label) {
        for (size_t i = 0; i < labels.size(); ++i) {
            if (labels[i] == label) return static_cast<int>(i);
        }
        return -1;
    };
    const int leftArm = partIndex("left arm");
    const int rightFoot = partIndex("right foot");
    const int heart = partIndex("heart");
    CHECK((leftArm >= 0 && rightFoot >= 0 && heart >= 0));

    // Five hundred pawns per part, each with a nearly healed injury that was once large (14 hit points).
    // The scar chance for those is 2%.
    const int parts[3] = {leftArm, rightFoot, heart};
    std::vector<Entity> pawns[3];
    int n = 0;
    for (int g = 0; g < 3; ++g) {
        for (int i = 0; i < 500; ++i, ++n) {
            const Entity pawn = sim.things().spawn("Human", CellPos{2 + n % 55, 2 + n / 55, 0});
            std::string error;
            PawnLooks looks;
            generateLooks(*defs, static_cast<uint64_t>(n + 1), false, "Baseline", looks, error);
            sim.things().registry().emplace_or_replace<PawnLooks>(pawn, looks);
            Health& health = sim.things().registry().get<Health>(pawn);
            health.injuries.push_back(Injury{static_cast<uint16_t>(parts[g]), cut, 0.05f, -1.0f, 14.0f});
            health.dirty = true;
            pawns[g].push_back(pawn);
        }
    }
    for (int i = 0; i < 1000; ++i) {
        sim.step();
    }
    int armMarks = 0;
    int footMarks = 0;
    int heartScars = 0;
    for (int g = 0; g < 3; ++g) {
        for (const Entity pawn : pawns[g]) {
            const Health& health = sim.things().registry().get<Health>(pawn);
            int scarInjuries = 0;
            for (const Injury& injury : health.injuries) {
                scarInjuries += injury.hediff == scarDef || injury.hediff == oldWound;
            }
            const PawnLooks& looks = *sim.looksOf(pawn);
            if (g == 2) {
                heartScars += scarInjuries;
                CHECK(looks.scars.empty());
                continue;
            }
            CHECK(looks.scars.size() == static_cast<size_t>(scarInjuries));
            for (const LooksMark& m : looks.scars) {
                CHECK(m.region == (g == 0 ? BodyRegion::LeftArm : BodyRegion::RightLeg));
                CHECK(m.style < 6);
                (g == 0 ? armMarks : footMarks) += 1;
            }
        }
    }
    CHECK(armMarks >= 1);
    CHECK(footMarks >= 1);
    CHECK(heartScars >= 1);
}

TEST_CASE("looks: scar styles follow the injury and the number of scars is capped") {
    const auto defs = looksTestDefs();
    PawnLooks looks;
    std::string error;
    CHECK(generateLooks(*defs, 3, false, "Baseline", looks, error));
    Rng rng(5, 1);
    for (int i = 0; i < 40; ++i) {
        addScarMark(*defs, looks, BodyRegion::Torso, "Burn", rng);
    }
    CHECK(looks.scars.size() == 24);  // LooksRules scars.max
    for (const LooksMark& m : looks.scars) {
        CHECK(m.style == 2);  // the Burn style
    }
    PawnLooks other = looks;
    other.scars.clear();
    for (int i = 0; i < 40; ++i) {
        addScarMark(*defs, other, BodyRegion::LeftArm, "SomethingElse", rng);
    }
    for (const LooksMark& m : other.scars) {
        CHECK(m.style == 5);  // only the style that names no injury
    }
    BodyRegion region = BodyRegion::Torso;
    CHECK(bodyRegionOfPart("Hand", true, region));
    CHECK(region == BodyRegion::LeftArm);
    CHECK(bodyRegionOfPart("Toe", false, region));
    CHECK(region == BodyRegion::RightLeg);
    CHECK(bodyRegionOfPart("Nose", false, region));
    CHECK(region == BodyRegion::Face);
    CHECK_FALSE(bodyRegionOfPart("Heart", false, region));
    CHECK_FALSE(bodyRegionOfPart("Ribcage", false, region));
}

TEST_CASE("looks: faces, marks and hair survive a save and load") {
    const auto defs = looksTestDefs();
    Sim sim(looksCrashlanded(), defs);
    const Entity pawn = *sim.things().registry().view<const Colonist>().begin();
    PawnLooks* looks = sim.things().registry().try_get<PawnLooks>(pawn);
    CHECK(looks != nullptr);
    if (looks == nullptr) return;
    CHECK_FALSE(looks->hairStyle.empty());
    looks->tattoos.push_back(LooksMark{BodyRegion::RightArm, 4, 0.25f, 0.75f, 1.1f});
    looks->scars.push_back(LooksMark{BodyRegion::Face, 1, 0.5f, 0.125f, 0.9f});
    const PawnLooks before = *looks;
    const std::vector<uint8_t> bytes = saveSim(sim);
    SaveLoadResult loaded = loadSim(bytes, defs);
    CHECK(loaded.error.empty());
    if (loaded.sim == nullptr) {
        CHECK(loaded.sim != nullptr);
        return;
    }
    CHECK(loaded.sim->stateHash() == sim.stateHash());
    CHECK(saveSim(*loaded.sim) == bytes);
    const PawnLooks* after = loaded.sim->looksOf(pawn);
    CHECK(after != nullptr);
    if (after == nullptr) return;
    CHECK(after->hairStyle == before.hairStyle);
    CHECK(after->beard == before.beard);
    CHECK(after->faceShape == before.faceShape);
    CHECK(after->jaw == before.jaw);
    CHECK(looksSameColor(after->eyes, before.eyes));
    CHECK(after->tattoos.size() == 1);
    CHECK(after->scars.size() == 1);
    CHECK(after->tattoos[0].region == BodyRegion::RightArm);
    CHECK(after->tattoos[0].style == 4);
    CHECK(after->scars[0].v == 0.125f);

    // The snapshot carries them to Unreal.
    Snapshot snap;
    sim.writeSnapshot(snap);
    bool seen = false;
    for (const ThingView& view : snap.things) {
        if (view.id == pawn && view.looks >= 0) {
            const PawnLooksView& v = snap.looks[static_cast<size_t>(view.looks)];
            seen = v.hairStyle == before.hairStyle && v.tattoos.size() == 1 && v.scars.size() == 1;
        }
    }
    CHECK(seen);
}

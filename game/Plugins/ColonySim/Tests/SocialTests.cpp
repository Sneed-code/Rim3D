#include "ColonySim/Colonist.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Social.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// These tests check docs/design/social.md (task C20).

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define SOC_REQUIRE(...)                  \
    do {                                  \
        const bool socChecked = (__VA_ARGS__); \
        CHECK(socChecked);                \
        if (!socChecked) {                \
            return;                       \
        }                                 \
    } while (0)

namespace {

// The game's real data: the interactions, thoughts, traits and bonds are what is tested.
std::shared_ptr<const DefDatabase> socDefs() {
    static std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        return std::shared_ptr<const DefDatabase>(db);
    }();
    return defs;
}

Sim socSim() { return Sim(SimSettings{5, 48, 0, 0, true}, socDefs()); }

// A colonist with a Colonist record (an adult with the Social skill at 0 and the given traits) that works
// all day, so it is awake and talks.
Entity socPawn(Sim& sim, CellPos cell, std::vector<std::string> traits = {}) {
    const Entity pawn = sim.things().spawn("Human", cell);
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.age = 25;
    colonist.skills.push_back(SkillRecord{"Social", 0, Passion::None, 0.0f, false});
    for (const std::string& trait : traits) {
        colonist.traits.push_back(TraitRecord{trait, 0});
    }
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    for (int h = 0; h < 24; ++h) {
        sim.submit(SetScheduleCommand{pawn, h, ScheduleKind::Work});
    }
    return pawn;
}

// Makes the components, which come in the first step.
void socStart(Sim& sim) { sim.step(); }

// Makes the components and keeps everyone quiet for 100 days, so only the talks a test asks for happen.
void socStartQuiet(Sim& sim) {
    sim.step();
    entt::registry& reg = sim.things().registry();
    for (const Entity pawn : reg.view<Social>()) {
        reg.get<Social>(pawn).nextTalk = sim.time().seconds + 100 * kSecondsPerDay;
    }
}

Social& socOf(Sim& sim, Entity pawn) { return sim.things().registry().get<Social>(pawn); }

int socCountEvents(Sim& sim, EventType type, const std::string& message = std::string()) {
    int n = 0;
    for (const Event& e : sim.events()) {
        if (e.type == type && (message.empty() || e.message == message)) {
            ++n;
        }
    }
    return n;
}

bool socThoughtHas(Sim& sim, Entity pawn, const std::string& def) {
    for (const ThoughtView& t : sim.mood().thoughts(pawn, sim.time().seconds)) {
        if (t.def == def) {
            return true;
        }
    }
    return false;
}

// A memory the pawn holds about another, set directly to put an opinion where a test needs it.
void socThinks(Sim& sim, Entity pawn, Entity other, float offset, const char* label = "test") {
    socOf(sim, pawn).memories.push_back(OpinionMemory{other, label, offset, sim.time().seconds + 100 * kSecondsPerDay});
}

// Submits a talk and steps once; returns whether it was refused.
bool socTalk(Sim& sim, Entity from, Entity to, const char* interaction, int outcome = -1) {
    sim.submit(InteractCommand{from, to, interaction, outcome});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::CommandFailed) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Social: colonists get a social life, and two strangers think about as well of each other") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStart(sim);
    SOC_REQUIRE(sim.things().registry().all_of<Social>(a) && sim.things().registry().all_of<Social>(b));
    const SocialRules& rules = sim.social().rules();
    SOC_REQUIRE(rules.present);
    CHECK(rules.interactions.size() == 8);
    // The first impression is the pair's compatibility: the same both ways, within the rules' range.
    const float ab = sim.social().opinion(a, b);
    const float ba = sim.social().opinion(b, a);
    CHECK(ab == doctest::Approx(ba));
    CHECK(std::fabs(ab) <= rules.compatibilityMax + 0.001f);
    CHECK(sim.social().opinion(a, a) == 0.0f);
    CHECK(sim.social().partner(a) == kNoEntity);
    CHECK_FALSE(sim.social().fighting(a));
    // It shows in the snapshot as no partner and no fight.
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& view : snap.things) {
        if (view.id == a) {
            CHECK(view.partner == kNoEntity);
            CHECK_FALSE(view.socialFight);
        }
    }
}

TEST_CASE("Social: chitchat changes the listener's opinion, starts a thought and teaches the talker") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStartQuiet(sim);
    const float before = sim.social().opinion(b, a);
    const float aBefore = sim.social().opinion(a, b);
    SOC_REQUIRE(socTalk(sim, a, b, "Chitchat"));
    CHECK(socCountEvents(sim, EventType::Interaction, "Chitchat") == 1);
    // Chitchat is +1 on the listener's opinion of the talker, not the other way.
    CHECK(sim.social().opinion(b, a) == doctest::Approx(before + 1.0f));
    CHECK(sim.social().opinion(a, b) == doctest::Approx(aBefore));
    CHECK(socThoughtHas(sim, a, "ChitchatHeard"));
    CHECK(socThoughtHas(sim, b, "ChitchatHeard"));
    // Only the talker learns: Social skill experience (10 for chitchat, at the no-passion rate).
    const Colonist& talker = sim.things().registry().get<Colonist>(a);
    CHECK(talker.skill("Social")->xp > 0.0f);
    CHECK(sim.things().registry().get<Colonist>(b).skill("Social")->xp == 0.0f);
    CHECK(sim.social().stats().chitchats == 1);
    CHECK(sim.social().stats().talks == 1);
}

TEST_CASE("Social: a kind of memory stacks to the limit and fades after its days") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStartQuiet(sim);
    sim.mentalBreaks().setNatural(false);  // 16 days without food would break them
    const float base = sim.social().opinion(b, a);
    for (int i = 0; i < 6; ++i) {
        SOC_REQUIRE(socTalk(sim, a, b, "Chitchat"));
    }
    // Six chats, but three at most are held (SocialRules.stackLimit).
    CHECK(sim.social().opinion(b, a) == doctest::Approx(base + 3.0f));
    CHECK(sim.social().opinionLines(b, a).size() == 4);  // compatibility and three chats
    // Chitchat lasts 15 days.
    sim.stepMany(16 * kSecondsPerDay);
    CHECK(sim.social().opinion(b, a) == doctest::Approx(base));
}

TEST_CASE("Social: slights and insults lower the opinion, kind words need a low mood, deep talk raises it") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStartQuiet(sim);
    const float base = sim.social().opinion(b, a);
    SOC_REQUIRE(socTalk(sim, a, b, "Slight"));
    CHECK(sim.social().opinion(b, a) == doctest::Approx(base - 5.0f));
    CHECK(socThoughtHas(sim, b, "Slighted"));
    SOC_REQUIRE(socTalk(sim, a, b, "Insult", 0));  // no fight
    CHECK(sim.social().opinion(b, a) == doctest::Approx(base - 25.0f));
    CHECK(socThoughtHas(sim, b, "Insulted"));
    CHECK_FALSE(sim.social().fighting(a));
    SOC_REQUIRE(socTalk(sim, a, b, "DeepTalk"));
    CHECK(sim.social().opinion(b, a) == doctest::Approx(base - 19.0f));
    CHECK(socThoughtHas(sim, a, "DeepTalkHeard"));
    CHECK(sim.social().stats().slights == 1);
    CHECK(sim.social().stats().insults == 1);
    CHECK(sim.social().stats().deepTalks == 1);
}

TEST_CASE("Social: pawns out of earshot, in another room, asleep or downed cannot talk") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity far = socPawn(sim, CellPos{40, 20, 0});
    // A walled room around a third pawn.
    for (int y = 28; y <= 34; ++y) {
        for (int x = 20; x <= 26; ++x) {
            if (x == 20 || x == 26 || y == 28 || y == 34) {
                sim.things().spawn("Wall", CellPos{x, y, 0});
            }
        }
    }
    const Entity inside = socPawn(sim, CellPos{23, 31, 0});
    const Entity near = socPawn(sim, CellPos{22, 20, 0});
    sim.rooms().update();
    socStart(sim);
    CHECK_FALSE(socTalk(sim, a, far, "Chitchat"));     // 20 m apart
    CHECK_FALSE(socTalk(sim, a, inside, "Chitchat"));  // 11 m, and a wall
    CHECK_FALSE(socTalk(sim, a, a, "Chitchat"));
    CHECK_FALSE(socTalk(sim, a, near, "NoSuchTalk"));
    SOC_REQUIRE(socTalk(sim, a, near, "Chitchat"));
    // A downed pawn does not talk.
    sim.things().registry().get<Health>(near).downed = true;
    CHECK_FALSE(socTalk(sim, a, near, "Chitchat"));
}

TEST_CASE("Social: pawns beside each other talk at about the rules' rate") {
    Sim sim = socSim();
    socPawn(sim, CellPos{20, 20, 0});
    socPawn(sim, CellPos{22, 20, 0});
    socPawn(sim, CellPos{20, 22, 0});
    socStart(sim);
    sim.stepMany(3 * kSecondsPerDay);
    // Three pawns, each starting a talk every six hours or so, with a 20 minute pause after any: about 36
    // talks in three days if they were awake all the time.
    const uint64_t talks = sim.social().stats().talks;
    CHECK(talks >= 12);
    CHECK(talks <= 80);
    CHECK(sim.social().stats().chitchats > sim.social().stats().deepTalks);
}

TEST_CASE("Social: romance is a bond only, one at a time, and marriage needs a lover") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    const Entity c = socPawn(sim, CellPos{20, 22, 0});
    socStartQuiet(sim);
    // No proposal without a lover.
    CHECK_FALSE(socTalk(sim, a, b, "MarriageProposal", 1));
    // A romantic gesture that fails leaves no bond.
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 0));
    CHECK(sim.social().bondBetween(a, b) == nullptr);
    CHECK(socCountEvents(sim, EventType::Interaction, "RomanceAttempt") == 1);
    // One that works makes them lovers, for both.
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 1));
    SOC_REQUIRE(sim.social().bondBetween(a, b) != nullptr);
    CHECK(sim.social().bondBetween(a, b)->kind == BondKind::Lover);
    CHECK(sim.social().bondBetween(b, a)->kind == BondKind::Lover);
    CHECK(sim.social().partner(a) == b);
    CHECK(sim.social().partner(b) == a);
    CHECK(socCountEvents(sim, EventType::BondChanged, "Lover") == 1);
    // A lover is thought of more kindly: +40 over what the two would otherwise think (the gesture adds 10).
    CHECK(sim.social().opinion(a, b) >= sim.social().opinion(a, c) + 30.0f - 10.0f);
    // Nobody can court someone who is already together with another (the partner may stray: see cheating).
    CHECK_FALSE(socTalk(sim, c, b, "RomanceAttempt", 1));
    // A refused proposal costs the asker a thought and changes no bond.
    SOC_REQUIRE(socTalk(sim, a, b, "MarriageProposal", 0));
    CHECK(sim.social().bondBetween(a, b)->kind == BondKind::Lover);
    CHECK(socThoughtHas(sim, a, "RejectedMyProposal"));
    CHECK_FALSE(socThoughtHas(sim, b, "RejectedMyProposal"));
    // An accepted one makes them spouses and both glad.
    SOC_REQUIRE(socTalk(sim, b, a, "MarriageProposal", 1));
    CHECK(sim.social().bondBetween(a, b)->kind == BondKind::Spouse);
    CHECK(sim.social().partner(a) == b);
    CHECK(socThoughtHas(sim, a, "GotMarried"));
    CHECK(socThoughtHas(sim, b, "GotMarried"));
    CHECK(sim.social().stats().weddings == 1);
    // A spouse does not break up (a breakup needs a lover).
    CHECK_FALSE(socTalk(sim, a, b, "Breakup"));
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& view : snap.things) {
        if (view.id == a) {
            CHECK(view.partner == b);
        }
    }
}

TEST_CASE("Social: a breakup makes ex-lovers, who think worse of each other and can begin again") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 1));
    SOC_REQUIRE(socTalk(sim, a, b, "Breakup"));
    SOC_REQUIRE(sim.social().bondBetween(a, b) != nullptr);
    CHECK(sim.social().bondBetween(a, b)->kind == BondKind::ExLover);
    CHECK(sim.social().bondBetween(b, a)->kind == BondKind::ExLover);
    CHECK(sim.social().partner(a) == kNoEntity);
    CHECK(socThoughtHas(sim, a, "BrokeUp"));
    CHECK(socThoughtHas(sim, b, "BrokeUp"));
    CHECK(socCountEvents(sim, EventType::BondChanged, "ExLover") == 1);
    CHECK(sim.social().stats().breakups == 1);
    // The ex-lover penalty and the breakup itself sit in the lines.
    bool exLine = false;
    for (const OpinionLine& line : sim.social().opinionLines(b, a)) {
        exLine = exLine || (line.label == "ExLover" && line.offset == -25.0f);
    }
    CHECK(exLine);
    // They may try again.
    SOC_REQUIRE(socTalk(sim, b, a, "RomanceAttempt", 1));
    CHECK(sim.social().bondBetween(a, b)->kind == BondKind::Lover);
}

TEST_CASE("Social: friends need both to think well of each other, a rival is one who is thought badly of") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    socStartQuiet(sim);
    socThinks(sim, a, b, 60.0f);
    CHECK_FALSE(sim.social().friends(a, b));  // b does not think so well of a
    socThinks(sim, b, a, 60.0f);
    CHECK(sim.social().friends(a, b));
    CHECK(sim.social().friends(b, a));
    CHECK_FALSE(sim.social().rival(a, b));
    socOf(sim, a).memories.clear();
    socThinks(sim, a, b, -90.0f);
    CHECK(sim.social().rival(a, b));
    CHECK_FALSE(sim.social().friends(a, b));
    // Opinion is held between -100 and 100.
    socThinks(sim, a, b, -500.0f);
    CHECK(sim.social().opinion(a, b) == -100.0f);
}

TEST_CASE("Social: traits change what others think and what a pawn says") {
    Sim sim = socSim();
    const Entity plain = socPawn(sim, CellPos{20, 20, 0});
    const Entity rude = socPawn(sim, CellPos{22, 20, 0}, {"Abrasive"});
    const Entity kind = socPawn(sim, CellPos{20, 22, 0}, {"Kind"});
    socStart(sim);
    // Everyone thinks 20 less of an abrasive pawn than the compatibility alone says.
    float traitLine = 0.0f;
    for (const OpinionLine& line : sim.social().opinionLines(plain, rude)) {
        if (line.label == "what they are like") {
            traitLine = line.offset;
        }
    }
    CHECK(traitLine == -20.0f);
    // A kind pawn never slights or insults, however low it thinks of the other; an abrasive one does so often.
    socThinks(sim, kind, plain, -50.0f);
    socThinks(sim, kind, rude, -50.0f);
    socThinks(sim, rude, plain, -50.0f);
    socThinks(sim, rude, kind, -50.0f);
    socThinks(sim, plain, rude, -50.0f);
    int fromKind = 0;
    int fromRude = 0;
    for (int i = 0; i < 6 * 3600; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            if (e.type == EventType::Interaction && (e.message == "Slight" || e.message == "Insult")) {
                fromKind += e.thing == kind ? 1 : 0;
                fromRude += e.thing == rude ? 1 : 0;
            }
        }
    }
    CHECK(fromKind == 0);
    CHECK(fromRude > 0);
}

TEST_CASE("Social: when someone dies, those who cared think about it, and a widow is free again") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    const Entity c = socPawn(sim, CellPos{20, 22, 0});
    const Entity d = socPawn(sim, CellPos{22, 22, 0});
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 1));
    SOC_REQUIRE(socTalk(sim, a, b, "MarriageProposal", 1));
    socThinks(sim, c, a, 60.0f);
    socThinks(sim, a, c, 60.0f);
    socThinks(sim, d, a, -80.0f);
    sim.step();
    // a dies: b (spouse), c (friend) and d (rival) each get their own thought.
    sim.things().registry().get<Health>(a).dead = true;
    sim.step();
    sim.step();
    CHECK(socThoughtHas(sim, b, "SpouseDied"));
    CHECK_FALSE(socThoughtHas(sim, b, "FriendDied"));
    CHECK(socThoughtHas(sim, c, "FriendDied"));
    CHECK(socThoughtHas(sim, d, "RivalDied"));
    CHECK(sim.social().partner(b) == kNoEntity);
    // Once only: the thoughts are not started again every second.
    int64_t expires = 0;
    for (const ThoughtView& t : sim.mood().thoughts(b, sim.time().seconds)) {
        if (t.def == "SpouseDied") {
            expires = t.expires;
        }
    }
    CHECK(expires > 0);
    sim.stepMany(600);
    for (const ThoughtView& t : sim.mood().thoughts(b, sim.time().seconds)) {
        if (t.def == "SpouseDied") {
            CHECK(t.expires == expires);
        }
    }
    // The widow can love again.
    SOC_REQUIRE(socTalk(sim, c, b, "RomanceAttempt", 1));
    CHECK(sim.social().partner(b) == c);
}

TEST_CASE("Social: an insult can start a fight with fists that ends with the fighters back to work") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{21, 20, 0});
    socStartQuiet(sim);
    const float base = sim.social().opinion(b, a);
    SOC_REQUIRE(socTalk(sim, a, b, "Insult", 1));
    CHECK(socCountEvents(sim, EventType::SocialFight, "started") == 1);
    SOC_REQUIRE(sim.social().fighting(a));
    CHECK(sim.social().fighting(b));
    CHECK(sim.social().stats().fights == 1);
    // While they fight, the job system leaves them alone.
    Snapshot snap;
    sim.writeSnapshot(snap);
    for (const ThingView& view : snap.things) {
        if (view.id == a || view.id == b) {
            CHECK(view.socialFight);
        }
    }
    int swings = 0;
    bool ended = false;
    for (int i = 0; i < 400 && !ended; ++i) {
        sim.step();
        for (const Event& e : sim.events()) {
            swings += e.type == EventType::MeleeSwing ? 1 : 0;
            if (e.type == EventType::SocialFight && e.message.rfind("ended", 0) == 0) {
                ended = true;
            }
        }
        if (!ended) {
            CHECK(sim.things().registry().get<Worker>(a).job.type == JobType::None);
        }
    }
    CHECK(ended);
    CHECK(swings > 0);
    CHECK_FALSE(sim.social().fighting(a));
    CHECK_FALSE(sim.social().fighting(b));
    // Nobody dies in a fistfight, and each thinks worse of the other afterwards (the insult and the fight).
    CHECK_FALSE(sim.things().registry().get<Health>(a).dead);
    CHECK_FALSE(sim.things().registry().get<Health>(b).dead);
    CHECK(sim.social().opinion(b, a) <= base - 20.0f - 20.0f + 0.01f);
    CHECK(sim.social().opinion(a, b) < sim.social().opinion(b, a) + 20.0f);
}

TEST_CASE("Social: a fight stops when the pawns are called away to fight for the colony") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{26, 20, 0});
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "Insult", 1));
    SOC_REQUIRE(sim.social().fighting(a));
    sim.submit(DraftCommand{a, true});
    sim.step();
    sim.step();
    CHECK_FALSE(sim.social().fighting(a));
    CHECK_FALSE(sim.social().fighting(b));
    CHECK(sim.things().registry().all_of<Fighter>(a));
}

TEST_CASE("Social: opinions, bonds and fights are saved and a loaded game goes on the same") {
    Sim original = socSim();
    const Entity a = socPawn(original, CellPos{20, 20, 0});
    const Entity b = socPawn(original, CellPos{21, 20, 0});
    const Entity c = socPawn(original, CellPos{20, 22, 0});
    socStartQuiet(original);
    SOC_REQUIRE(socTalk(original, a, b, "RomanceAttempt", 1));
    SOC_REQUIRE(socTalk(original, c, a, "Slight"));
    SOC_REQUIRE(socTalk(original, c, b, "Insult", 1));  // a fight
    original.stepMany(40);
    SOC_REQUIRE(original.social().fighting(c));

    const std::vector<uint8_t> bytes = saveSim(original);
    const SaveLoadResult loaded = loadSim(bytes, socDefs());
    SOC_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == original.stateHash());
    CHECK(copy.social().fighting(c));
    CHECK(copy.social().partner(a) == b);
    CHECK(copy.social().opinion(a, c) == doctest::Approx(original.social().opinion(a, c)));
    CHECK(copy.social().opinion(b, c) == doctest::Approx(original.social().opinion(b, c)));
    // Both play on to the same state, including the end of the fight and every later talk.
    for (int i = 0; i < 3 * 3600; ++i) {
        original.step();
        copy.step();
    }
    CHECK(copy.stateHash() == original.stateHash());
    // And saving again gives the same bytes.
    CHECK(saveSim(copy) == saveSim(original));
}

TEST_CASE("Social: the talk command is replayed, saved by name and known by name") {
    Sim original = socSim();
    const Entity a = socPawn(original, CellPos{20, 20, 0});
    const Entity b = socPawn(original, CellPos{22, 20, 0});
    socStartQuiet(original);
    original.submit(InteractCommand{a, b, "RomanceAttempt", 1});
    original.submit(InteractCommand{b, a, "MarriageProposal", -1});
    original.submit(InteractCommand{a, b, "Insult", 1});
    original.stepMany(900);

    const Replay recorded = recordReplay(original);
    std::string error;
    Replay parsed;
    SOC_REQUIRE(readReplay(writeReplay(recorded), parsed, error));
    CHECK(parsed.commands.size() == recorded.commands.size());
    CHECK(writeReplay(recorded).find("\"Interact\"") != std::string::npos);
    Command command;
    CHECK(commandFromName("Interact", command));
    CHECK(std::string(commandName(command)) == "Interact");
}

TEST_CASE("Social: a skilled talker's good chat counts for more and romance is likelier") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    const Entity c = socPawn(sim, CellPos{20, 22, 0});
    socStartQuiet(sim);
    sim.things().registry().get<Colonist>(b).skills[0].level = 10;
    const float before = sim.social().opinion(c, a);
    SOC_REQUIRE(socTalk(sim, a, c, "DeepTalk", 1));
    const float plain = sim.social().opinion(c, a) - before;
    const float before2 = sim.social().opinion(c, b);
    SOC_REQUIRE(socTalk(sim, b, c, "DeepTalk", 1));
    const float skilled = sim.social().opinion(c, b) - before2;
    CHECK(plain == doctest::Approx(6.0f));
    CHECK(skilled == doctest::Approx(6.0f * (1.0f + 10.0f * sim.social().rules().skillOpinionPerLevel)));
    // A slight is no softer for skill.
    const float before3 = sim.social().opinion(c, b);
    SOC_REQUIRE(socTalk(sim, b, c, "Slight", 1));
    CHECK(sim.social().opinion(c, b) - before3 == doctest::Approx(-5.0f));
}

TEST_CASE("Social: a lover who strays is cheating; the betrayed one is upset and may fight the new lover") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0}, {"Kind"});  // too kind to fight
    const Entity c = socPawn(sim, CellPos{20, 22, 0});
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 1));
    const float before = sim.social().opinion(b, a);
    SOC_REQUIRE(socTalk(sim, a, c, "RomanceAttempt", 1));
    const SocialRules& rules = sim.social().rules();
    // Both bonds stand, the betrayed one thinks worse of both and is upset.
    CHECK(sim.social().bondBetween(a, b) != nullptr);
    CHECK(sim.social().bondBetween(a, c) != nullptr);
    CHECK(sim.social().stats().cheats == 1);
    CHECK(sim.social().opinion(b, a) == doctest::Approx(before + rules.cheatedOffset));
    CHECK(sim.social().opinion(b, c) <= sim.social().opinion(c, b) + rules.tookPartnerOffset + 20.0f);
    CHECK(socThoughtHas(sim, b, "CheatedOnMe"));
    CHECK_FALSE(socThoughtHas(sim, a, "CheatedOnMe"));
    CHECK(socCountEvents(sim, EventType::Interaction, "Cheated") == 1);
    CHECK_FALSE(sim.social().fighting(b));
    // The one asked must be free.
    CHECK_FALSE(socTalk(sim, c, b, "RomanceAttempt", 1));
}

TEST_CASE("Social: a wedding is a party: the guests and the couple get their thoughts") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{22, 20, 0});
    const Entity guest = socPawn(sim, CellPos{20, 22, 0});
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "RomanceAttempt", 1));
    CHECK_FALSE(socThoughtHas(sim, guest, "AttendedWedding"));
    SOC_REQUIRE(socTalk(sim, a, b, "MarriageProposal", 1));
    CHECK(sim.social().stats().weddings == 1);
    CHECK(socThoughtHas(sim, a, "WeddingParty"));
    CHECK(socThoughtHas(sim, b, "WeddingParty"));
    CHECK(socThoughtHas(sim, a, "GotMarried"));
    CHECK(socThoughtHas(sim, guest, "AttendedWedding"));
    CHECK_FALSE(socThoughtHas(sim, a, "AttendedWedding"));
    CHECK(sim.social().stats().weddingGuests == 1);
}

TEST_CASE("Social: an insult never starts a fight with the protagonist") {
    Sim sim = socSim();
    const Entity a = socPawn(sim, CellPos{20, 20, 0});
    const Entity b = socPawn(sim, CellPos{21, 20, 0});
    sim.things().registry().get<Colonist>(b).protagonist = true;
    socStartQuiet(sim);
    SOC_REQUIRE(socTalk(sim, a, b, "Insult", 1));
    CHECK(socCountEvents(sim, EventType::SocialFight, "started") == 0);
    CHECK_FALSE(sim.social().fighting(a));
    CHECK_FALSE(sim.social().fighting(b));
}

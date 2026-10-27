#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"
#include "ColonySim/Grid.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace colony {

class FighterSystem;
class HealthSystem;
class Map;
class RaidSystem;
class Rng;
class StateHasher;
class Things;
struct GameTime;

// Quests (task Q1). See docs/design/quests.md.
//
// A quest is an offer from outside the colony: it says what is asked and what comes of it, waits a few days, and the player
// accepts or declines it (RimWorld's quest tab). Four kinds need no world travel: Refugees (people run to the colony with
// raiders behind them), Wanderer (one person asks to join), Stash (a cache of goods is dropped for the colony) and Defend (a
// raid is coming; beat it and be paid). The narrator offers quests through `QuestSystem::offer` / `offerRandom`, or the
// QuestCommand "offer". Unreal does not include this header: it reads `quests` from the snapshot and sends QuestCommand.

enum class QuestState : uint8_t {
    Offered,    // waiting for the player's answer
    Active,     // accepted and under way
    Completed,  // done; the reward was paid
    Failed,     // accepted, but it went wrong
    Declined,   // the player said no
    Expired,    // nobody answered in time
};

COLONY_API const char* questStateName(QuestState state);

// One quest, offered or finished. Every field is saved.
struct Quest {
    uint32_t id = 0;
    std::string kind;            // Quests defName
    QuestState state = QuestState::Offered;
    int64_t offeredAt = 0;       // game second
    int64_t expiresAt = 0;       // an unanswered offer lapses at this game second
    int64_t acceptedAt = 0;
    int64_t launchAt = 0;        // Refugees, Defend: when the raid starts
    int64_t deadline = 0;        // an active quest that is not done by this game second fails
    bool launched = false;       // the raid has been sent
    int count = 0;               // Refugees: how many people
    float points = 0.0f;         // Refugees, Defend: the raid's points
    std::string faction;         // Refugees, Defend: the raiders' Factions defName
    uint32_t squad = 0;          // the raid squad that was sent (0 until launched)
    std::vector<Entity> people;  // Refugees: who came; Wanderer: who joined
    struct Reward {
        std::string thing;       // Things defName
        int count = 0;
    };
    std::vector<Reward> rewards;
};

// Everything the quests keep between steps; saved as a whole (Save.cpp).
struct QuestSystemState {
    std::vector<Quest> quests;  // in id order, finished ones included (the newest `historyKept` of them)
    uint32_t next = 1;
};

// The numbers (QuestRules in data/defs/Rules/QuestRules.json). All ours: RimWorld's quests are far more varied.
struct COLONY_API QuestRules {
    bool present = false;
    int maxOffered = 4;             // offers waiting at one time
    int maxActive = 3;              // accepted quests under way at one time
    int historyKept = 20;           // finished quests kept in the list
    int rewardSpread = 6;           // cells round the colony's middle a reward may land in
    int deliverySearch = 800;       // cells looked at for room for a delivery
    int edgeDepth = 2;              // cells in from the map edge people walk in at
    static QuestRules read(const DefDatabase& defs);
};

class COLONY_API QuestSystem {
public:
    // Makes a colonist (a whole person, working for the player) at a cell; kNoEntity if it can't.
    using JoinMaker = std::function<Entity(CellPos cell, uint64_t seed)>;

    QuestSystem(Things& things, Map& map, RaidSystem& raids, FighterSystem& fighters, HealthSystem& health, const DefDatabase& defs,
                const GameTime& time, Rng& rng);

    const QuestRules& rules() const { return rules_; }
    void setEventLog(std::vector<Event>* events) { events_ = events; }
    void setJoinMaker(JoinMaker maker) { joinMaker_ = std::move(maker); }
    const QuestSystemState& state() const { return state_; }
    QuestSystemState& state() { return state_; }

    const Quest* find(uint32_t id) const;
    int count(QuestState state) const;

    // The quest kinds the colony could be offered now (earliest day, enough colonists, room in the lists), by Quests defName.
    std::vector<std::string> available() const;
    // The incident hooks for the narrator: make an offer of this kind / of a kind chosen by weight. False, with the reason in
    // plain words, when none can be made. `id` is the new quest's number.
    bool offer(const std::string& kind, uint32_t& id, std::string& error);
    bool offerRandom(uint32_t& id, std::string& error);

    // The player's answers. False, with a reason, if the quest isn't an open offer or can't be carried out now.
    bool accept(uint32_t id, std::string& error);
    bool decline(uint32_t id, std::string& error);

    // Runs once a step: offers lapse, raids are sent when their time comes, active quests are checked for success or failure.
    void step();

    void hashState(StateHasher& h) const;

private:
    int64_t now() const;
    Quest* findMutable(uint32_t id);
    void emit(EventType type, const Quest& quest, const char* message) const;
    void finish(Quest& quest, QuestState state, const char* message);
    void trim();
    CellPos colonyCentre() const;
    bool nearbyCells(CellPos around, size_t wanted, std::vector<CellPos>& out) const;
    bool edgeCells(size_t wanted, std::vector<CellPos>& out) const;
    bool pay(const Quest& quest);
    bool launchRaid(Quest& quest);
    bool alive(Entity pawn) const;
    bool peopleArrive(Quest& quest, int count);

    Things& things_;
    Map& map_;
    RaidSystem& raids_;
    FighterSystem& fighters_;
    HealthSystem& health_;
    const DefDatabase& defs_;
    const GameTime& time_;
    Rng& rng_;
    QuestRules rules_;
    QuestSystemState state_;
    JoinMaker joinMaker_;
    std::vector<Event>* events_ = nullptr;
};

}  // namespace colony

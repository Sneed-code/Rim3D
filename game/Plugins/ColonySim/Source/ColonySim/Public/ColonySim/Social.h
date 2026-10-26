#pragma once

#include "ColonySim/Api.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Entity.h"
#include "ColonySim/Events.h"

#include <cstdint>
#include <string>
#include <vector>

namespace colony {

struct GameTime;
class HealthSystem;
class JobSystem;
class MeleeSystem;
class MoodSystem;
class Rooms;
class StateHasher;
class Things;

// Social life (task C20). See docs/design/social.md.
//
// Colonists talk. Every pawn that has someone in earshot starts an interaction now and then (chitchat,
// deep talk, kind words, a slight or an insult, a romantic gesture, a proposal, a breakup); each one
// changes how the other pawn thinks of it (an *opinion*, a number from -100 to 100 kept as memories
// that fade), may leave a mood thought, and gives the talker Social experience. Friends and rivals are
// worked out from opinions; lovers and spouses (and their exes) are bonds that are stored. Romance and
// marriage are bonds only: no sex, no pregnancy. An insult can start a fight with fists that ends
// when someone is hurt.
//
// Unreal does not include this header: it reads the snapshot's `partner` and `socialFight`, the
// `Interaction`, `BondChanged` and `SocialFight` events, and the opinions through `Sim::social()`.

enum class BondKind : uint8_t { Lover, Spouse, ExLover, ExSpouse };

COLONY_API const char* bondKindName(BondKind kind);

// What one pawn thinks of another because of something that happened (a chat, a slight...). The
// owner of the component holds it; `other` is the pawn it is about.
struct OpinionMemory {
    Entity other = kNoEntity;
    std::string label;     // "chitchat", "insulted me"
    float offset = 0.0f;
    int64_t expires = 0;   // game second it fades
};

// A bond that is stored (friends and rivals are not: they follow from opinions). Held by both pawns.
struct Bond {
    BondKind kind = BondKind::Lover;
    Entity other = kNoEntity;
    int64_t since = 0;
};

// On every colonist once it first steps (made with its Mood). Every field is saved.
struct Social {
    std::vector<OpinionMemory> memories;
    std::vector<Bond> bonds;
    int64_t nextTalk = 0;      // game second from which it may start an interaction (a pause after any)
    bool mourned = false;      // dead, and the ones who cared have been told (once)
};

// On both pawns of a fight that started from an insult: they fight with their fists, one job-free
// pawn against the other, until someone is hurt enough, down, or time runs out. Every field is saved.
struct SocialFight {
    Entity opponent = kNoEntity;
    int64_t startedAt = 0;
    int64_t endsBy = 0;
    int64_t nextMove = 0;      // game second to walk towards the opponent again
};

// A line of the reasons for an opinion, for the colonist panel.
struct OpinionLine {
    std::string label;
    float offset = 0.0f;
};

// Who a pawn may fall in love with (ours): Straight likes the other gender, Gay the same, Bisexual either, Asexual nobody.
enum class Orientation : uint8_t { Straight, Gay, Bisexual, Asexual };

// An interaction def (Interactions) as the system reads it.
struct SocialInteraction {
    DefId def;
    std::string defName;
    float weight = 1.0f;
    std::string opinionLabel;      // what the recipient's opinion memory is called, empty for none
    float opinionOffset = 0.0f;
    float opinionDays = 0.0f;
    int stackLimit = 0;            // 0: SocialRules' stackLimit
    float xp = 0.0f;
    std::string initiatorTrigger;  // mood thought triggers, empty for none
    std::string recipientTrigger;
    bool hasMinOpinion = false;
    float minOpinion = 0.0f;
    bool hasMaxOpinion = false;
    float maxOpinion = 0.0f;
    std::string bond;              // "Lover", "Spouse", "ExLover" or empty
    float successBase = 1.0f;
    bool requiresAdult = false;
    bool requiresSingleOrOpen = false;
    bool requiresLover = false;
    std::string rejectedTrigger;
    std::string acceptedTrigger;
};

// The numbers from SocialRules (data/defs/Rules) and the interactions.
struct COLONY_API SocialRules {
    bool present = false;
    float friendOpinion = 20.0f;
    float rivalOpinion = -20.0f;
    float meanHours = 6.0f;              // between one pawn's talks, when somebody is in earshot
    float maxDistance = 6.0f;            // metres
    float opinionMin = -100.0f;
    float opinionMax = 100.0f;
    float defaultXp = 15.0f;             // Social experience when an interaction def has none
    // Ours.
    float minGapMinutes = 20.0f;         // after taking part in an interaction a pawn starts none for this long
    float tableChatMeanMinutes = 15.0f;  // between talks of a pawn playing or chatting beside another
    float compatibilityMax = 10.0f;      // two strangers' opinion of each other lies within +-this, from a hash of the pair
    int stackLimit = 3;                  // opinion memories of one kind a pawn holds about one other
    float kindWordsMoodBelow = 40.0f;    // kind words go to someone whose mood is under this
    float romanceOpinionScale = 100.0f;  // success chance = base * (1 + the target's opinion / this)
    int adultAge = 18;
    float fightChance = 0.1f;            // an insult starts a fight this often (twice for a Brawler)
    float fightPain = 0.25f;             // the fight ends when either has this much pain
    float fightBloodLoss = 0.1f;         // ...or this much blood loss
    float fightMaxMinutes = 5.0f;        // ...or after this long
    float fightWalkEvery = 2.0f;         // seconds between steps towards the opponent
    std::string fightLabel = "got in a fight with me";
    // Bonds: how a stored bond moves the opinion (the Bonds defs), and the mood thought's trigger when the
    // other pawn dies (the strongest of these only).
    float bondOpinion[4] = {40.0f, 60.0f, -25.0f, -25.0f};  // by BondKind
    std::string spouseDeath, loverDeath, friendDeath, rivalDeath;
    // Social skill (ours): each level adds this share to what a good chat gives and to the odds of romance.
    float skillOpinionPerLevel = 0.05f;
    float skillRomancePerLevel = 0.02f;
    // Cheating and jealousy (ours): a pawn with a lover or spouse whom it thinks less of than `cheatPartnerBelow`
    // makes romance attempts on others at `cheatWeightFactor` times the usual weight.
    float cheatWeightFactor = 0.15f;
    float cheatPartnerBelow = 60.0f;
    std::string cheatedLabel = "cheated on me", tookPartnerLabel = "took my partner";
    float cheatedOffset = -40.0f, cheatedDays = 60.0f, tookPartnerOffset = -20.0f, tookPartnerDays = 60.0f;
    float jealousFightChance = 0.25f;    // the betrayed one fights the new lover (not a Wimp or a Kind one)
    std::string cheatedThought;          // mood thought triggers; empty for none
    std::string weddingGuestThought, weddingPartyThought;
    // Orientation (ours), a share of pawns each, by Orientation; pawns without a gender (no Colonist record) like anybody.
    float orientationWeights[4] = {0.7f, 0.1f, 0.15f, 0.05f};
    // Seeing a loved one or a friend die (ours): whoever stands within this many metres in the same room, or both out in the open,
    // gets this thought on top of the loss.
    float witnessRadius = 12.0f;
    std::string witnessThought;
    float fightOffset = -20.0f;          // each one's opinion of the other afterwards...
    float fightDays = 30.0f;             // ...for this long
    std::vector<SocialInteraction> interactions;

    static SocialRules read(const DefDatabase& defs);
    const SocialInteraction* find(const std::string& defName) const;
};

// Keeps opinions, bonds, interactions and fights. One `step` per game second.
class COLONY_API SocialSystem {
public:
    SocialSystem(Things& things, Rooms& rooms, HealthSystem& health, MeleeSystem& melee, MoodSystem& mood,
                 JobSystem& jobs, const DefDatabase& defs, const GameTime& time, uint64_t seed);

    const SocialRules& rules() const { return rules_; }
    SocialRules& rules() { return rules_; }  // tests turn rules off or on

    // What `pawn` thinks of `other`, -100 to 100: compatibility, traits, bonds and memories. 0 for
    // pawns that take no part in social life.
    float opinion(Entity pawn, Entity other) const;
    // The same, with the reasons.
    std::vector<OpinionLine> opinionLines(Entity pawn, Entity other) const;
    // Friends: both think at least friendOpinion of each other. A rival: `pawn` thinks at most
    // rivalOpinion of `other`.
    bool friends(Entity a, Entity b) const;
    bool rival(Entity pawn, Entity other) const;
    // The stored bond between two pawns, or null.
    const Bond* bondBetween(Entity a, Entity b) const;
    // The pawn's spouse, else its lover, else none.
    Entity partner(Entity pawn) const;
    bool fighting(Entity pawn) const;
    // The pawn's orientation: from a hash of the pawn and the world seed, so it never changes and needs no saving.
    Orientation orientationOf(Entity pawn) const;
    // Whether `pawn` could fall in love with `other` (their genders against `pawn`'s orientation). One-sided; a romance
    // needs it both ways.
    bool attractedTo(Entity pawn, Entity other) const;

    // Starts `interaction` (an Interactions defName) from `initiator` to `recipient` now, as the dev
    // command and tests do. The opinion limits are ignored; what the interaction requires (a lover,
    // adults) and whether the two can talk (alive, awake, not downed, fighting or under the player's
    // hand, within earshot) are not. `outcome` 1 makes a roll succeed, 0 fail, -1 leaves it to the dice.
    bool interact(Entity initiator, Entity recipient, const std::string& interaction, int outcome, std::string& error);

    void setEventLog(std::vector<Event>* events) { events_ = events; }

    // One game second, after the job system and before the mood system: makes the components, tells
    // the ones who cared about a death, ends fights, walks the fighters, and rolls the talks.
    void step(int64_t now);

    // Hashes one thing's social components (Sim::stateHash calls it for every thing).
    void hashThing(Entity e, StateHasher& h) const;

    // Counters for tests and simrun.
    struct Stats {
        uint64_t talks = 0;       // interactions of every kind
        uint64_t chitchats = 0;
        uint64_t deepTalks = 0;
        uint64_t kindWords = 0;
        uint64_t slights = 0;
        uint64_t insults = 0;
        uint64_t romances = 0;    // attempts
        uint64_t lovers = 0;      // bonds formed
        uint64_t proposals = 0;
        uint64_t weddings = 0;
        uint64_t breakups = 0;
        uint64_t fights = 0;
        uint64_t cheats = 0;          // romances begun by somebody who already had a partner
        uint64_t weddingGuests = 0;   // thoughts of a wedding party given to the others
    };
    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }

private:
    struct Candidate {
        Entity other = kNoEntity;
        size_t interaction = 0;
        float weight = 0.0f;
    };

    bool takesPart(Entity pawn) const;
    bool canTalk(Entity pawn) const;
    bool inEarshot(Entity a, Entity b, float radius = -1.0f);
    float compatibility(Entity a, Entity b) const;
    float traitOpinion(Entity of) const;
    float traitWeight(Entity pawn, const std::string& interaction) const;
    bool hasTrait(Entity pawn, const char* trait) const;
    bool isAdult(Entity pawn) const;
    bool single(Entity pawn) const;
    bool eligible(Entity initiator, Entity recipient, const SocialInteraction& interaction, bool checkOpinion) const;
    void addMemory(Entity pawn, Entity other, const std::string& label, float offset, float days, int stackLimit);
    void setBond(Entity a, Entity b, BondKind kind);
    void dropBond(Entity a, Entity b, BondKind kind);
    void perform(Entity initiator, Entity recipient, const SocialInteraction& interaction, int outcome);
    void thought(Entity pawn, const std::string& trigger);
    void learn(Entity pawn, double xp);
    void jealousy(Entity betrayed, Entity cheater, Entity newLover);
    void weddingParty(Entity a, Entity b);
    float skillFactor(Entity pawn, float perLevel) const;
    void startFight(Entity a, Entity b);
    void endFight(Entity a, Entity b, const char* why);
    void stepFights();
    void mourn(Entity dead);
    void roll(Entity pawn, const std::vector<Entity>& pawns);
    double unit(Entity pawn, uint64_t salt) const;
    void push(EventType type, Entity a, Entity b, const std::string& message, int count, float amount);

    Things& things_;
    Rooms& rooms_;
    HealthSystem& health_;
    MeleeSystem& melee_;
    MoodSystem& mood_;
    JobSystem& jobs_;
    const DefDatabase& defs_;
    const GameTime& time_;
    uint64_t seed_;
    SocialRules rules_;
    Stats stats_;
    std::vector<Event>* events_ = nullptr;
    std::vector<Candidate> candidates_;      // scratch
    std::vector<Entity> near_;               // scratch
};

}  // namespace colony

#include "ColonySim/Social.h"

#include "ColonySim/Animals.h"
#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Melee.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Needs.h"
#include "ColonySim/Random.h"
#include "ColonySim/Rooms.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

float socFloat(const Def& def, const char* path, float fallback) {
    return static_cast<float>(def.number(path, fallback));
}

// The trait degree's def path ("degreeDatas.N") for a colonist's trait, or empty.
std::string socDegreePath(const Def& trait, int degree) {
    const size_t degrees = trait.count("degreeDatas");
    for (size_t i = 0; i < degrees; ++i) {
        const std::string path = "degreeDatas." + std::to_string(i);
        if (trait.integer((path + ".degree").c_str(), 0) == degree) {
            return path;
        }
    }
    return std::string();
}

// The trigger of a Thoughts def named by an Interactions or Bonds field, or empty.
std::string socTrigger(const DefDatabase& defs, const std::string& thought) {
    if (thought.empty()) {
        return std::string();
    }
    const Def* def = defs.find("Thoughts", thought);
    if (def == nullptr) {
        return std::string();
    }
    const std::string trigger = def->text("trigger");
    return trigger.empty() ? thought : trigger;
}

double socUnit(uint64_t h) {
    return static_cast<double>(splitMix64(h) >> 11) / 9007199254740992.0;
}

uint64_t socId(Entity e) {
    return static_cast<uint64_t>(entt::to_integral(e));
}

}  // namespace

const char* bondKindName(BondKind kind) {
    switch (kind) {
        case BondKind::Lover: return "Lover";
        case BondKind::Spouse: return "Spouse";
        case BondKind::ExLover: return "ExLover";
        case BondKind::ExSpouse: return "ExSpouse";
    }
    return "?";
}

// ---- Rules ----------------------------------------------------------------------------------

SocialRules SocialRules::read(const DefDatabase& defs) {
    SocialRules r;
    const Def* rules = defs.find("Rules", "SocialRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    r.friendOpinion = socFloat(*rules, "friendOpinion", r.friendOpinion);
    r.rivalOpinion = socFloat(*rules, "rivalOpinion", r.rivalOpinion);
    r.meanHours = std::max(0.01f, socFloat(*rules, "interactionMeanHours", r.meanHours));
    r.maxDistance = socFloat(*rules, "maxDistance", r.maxDistance);
    r.opinionMin = socFloat(*rules, "opinionMin", r.opinionMin);
    r.opinionMax = socFloat(*rules, "opinionMax", r.opinionMax);
    r.defaultXp = socFloat(*rules, "socialSkillXpPerInteraction", r.defaultXp);
    r.minGapMinutes = socFloat(*rules, "minGapMinutes", r.minGapMinutes);
    r.tableChatMeanMinutes = std::max(0.01f, socFloat(*rules, "tableChatMeanMinutes", r.tableChatMeanMinutes));
    r.compatibilityMax = socFloat(*rules, "compatibilityMax", r.compatibilityMax);
    r.stackLimit = std::max(1, static_cast<int>(rules->integer("stackLimit", r.stackLimit)));
    r.kindWordsMoodBelow = socFloat(*rules, "kindWordsMoodBelow", r.kindWordsMoodBelow);
    r.romanceOpinionScale = std::max(1.0f, socFloat(*rules, "romanceOpinionScale", r.romanceOpinionScale));
    r.adultAge = static_cast<int>(rules->integer("adultAge", r.adultAge));
    r.fightChance = socFloat(*rules, "fightChance", r.fightChance);
    r.fightPain = socFloat(*rules, "fightPain", r.fightPain);
    r.fightBloodLoss = socFloat(*rules, "fightBloodLoss", r.fightBloodLoss);
    r.fightMaxMinutes = socFloat(*rules, "fightMaxMinutes", r.fightMaxMinutes);
    r.fightWalkEvery = std::max(1.0f, socFloat(*rules, "fightWalkEvery", r.fightWalkEvery));
    r.fightLabel = rules->text("fight.label", r.fightLabel);
    r.fightOffset = socFloat(*rules, "fight.offset", r.fightOffset);
    r.fightDays = socFloat(*rules, "fight.days", r.fightDays);

    const char* kinds[4] = {"Lover", "Spouse", "ExLover", "ExSpouse"};
    for (int i = 0; i < 4; ++i) {
        if (const Def* bond = defs.find("Bonds", kinds[i])) {
            r.bondOpinion[i] = socFloat(*bond, "opinionOffset", r.bondOpinion[i]);
        }
    }
    const auto death = [&](const char* bond) {
        const Def* def = defs.find("Bonds", bond);
        return def != nullptr ? socTrigger(defs, def->text("deathThought")) : std::string();
    };
    r.spouseDeath = death("Spouse");
    r.loverDeath = death("Lover");
    r.friendDeath = death("Friend");
    r.rivalDeath = death("Rival");

    for (const DefId id : defs.idsOfKind("Interactions")) {
        const Def& def = defs.get(id);
        SocialInteraction i;
        i.def = id;
        i.defName = def.defName();
        i.weight = socFloat(def, "selectionWeight", 1.0f);
        i.opinionLabel = def.text("recipientOpinion.label");
        i.opinionOffset = socFloat(def, "recipientOpinion.offset", 0.0f);
        i.opinionDays = socFloat(def, "recipientOpinion.days", 0.0f);
        i.stackLimit = static_cast<int>(def.integer("recipientOpinion.stackLimit", 0));
        i.xp = socFloat(def, "socialXp", 0.0f);
        i.initiatorTrigger = socTrigger(defs, def.text("initiatorThought"));
        i.recipientTrigger = socTrigger(defs, def.text("recipientThought"));
        i.hasMinOpinion = def.has("minOpinion");
        i.minOpinion = socFloat(def, "minOpinion", 0.0f);
        i.hasMaxOpinion = def.has("maxOpinion");
        i.maxOpinion = socFloat(def, "maxOpinion", 0.0f);
        i.bond = def.text("bond");
        i.successBase = socFloat(def, "successBase", 1.0f);
        for (size_t k = 0; k < def.count("requires"); ++k) {
            const std::string need = def.textAt("requires", k);
            i.requiresAdult = i.requiresAdult || need == "adult";
            i.requiresSingleOrOpen = i.requiresSingleOrOpen || need == "single-or-open";
            i.requiresLover = i.requiresLover || need == "lover";
        }
        i.rejectedTrigger = socTrigger(defs, def.text("rejectedThought"));
        i.acceptedTrigger = socTrigger(defs, def.text("acceptedThought"));
        r.interactions.push_back(std::move(i));
    }
    return r;
}

const SocialInteraction* SocialRules::find(const std::string& defName) const {
    for (const SocialInteraction& i : interactions) {
        if (i.defName == defName) {
            return &i;
        }
    }
    return nullptr;
}

// ---- Setup and questions --------------------------------------------------------------------

SocialSystem::SocialSystem(Things& things, Rooms& rooms, HealthSystem& health, MeleeSystem& melee, MoodSystem& mood,
                           JobSystem& jobs, const DefDatabase& defs, const GameTime& time, uint64_t seed)
    : things_(things),
      rooms_(rooms),
      health_(health),
      melee_(melee),
      mood_(mood),
      jobs_(jobs),
      defs_(defs),
      time_(time),
      seed_(seed),
      rules_(SocialRules::read(defs)) {}

double SocialSystem::unit(Entity pawn, uint64_t salt) const {
    return socUnit(seed_ ^ (static_cast<uint64_t>(time_.seconds) * 0x9E3779B97F4A7C15ULL) ^
                   (socId(pawn) * 0xC2B2AE3D27D4EB4FULL) ^ (salt * 0x165667B19E3779F9ULL));
}

bool SocialSystem::hasTrait(Entity pawn, const char* trait) const {
    const Colonist* colonist = things_.registry().valid(pawn) ? things_.registry().try_get<Colonist>(pawn) : nullptr;
    if (colonist == nullptr) {
        return false;
    }
    for (const TraitRecord& record : colonist->traits) {
        if (record.def == trait) {
            return true;
        }
    }
    return false;
}

bool SocialSystem::isAdult(Entity pawn) const {
    const Colonist* colonist = things_.registry().valid(pawn) ? things_.registry().try_get<Colonist>(pawn) : nullptr;
    return colonist == nullptr || colonist->age >= rules_.adultAge;
}

// What everyone thinks of this pawn because of what it is: its traits' `opinionOfMe`.
float SocialSystem::traitOpinion(Entity of) const {
    const Colonist* colonist = things_.registry().valid(of) ? things_.registry().try_get<Colonist>(of) : nullptr;
    float total = 0.0f;
    if (colonist == nullptr) {
        return total;
    }
    for (const TraitRecord& record : colonist->traits) {
        if (const Def* trait = defs_.find("Traits", record.def)) {
            const std::string path = socDegreePath(*trait, record.degree);
            if (!path.empty()) {
                total += socFloat(*trait, (path + ".opinionOfMe").c_str(), 0.0f);
            }
        }
    }
    return total;
}

// How the talker's traits change an interaction's weight (SocialRules.traitWeights.<Trait>.<Interaction>).
float SocialSystem::traitWeight(Entity pawn, const std::string& interaction) const {
    const Colonist* colonist = things_.registry().valid(pawn) ? things_.registry().try_get<Colonist>(pawn) : nullptr;
    const Def* rules = defs_.find("Rules", "SocialRules");
    float factor = 1.0f;
    if (colonist == nullptr || rules == nullptr) {
        return factor;
    }
    for (const TraitRecord& record : colonist->traits) {
        const std::string path = "traitWeights." + record.def + "." + interaction;
        if (rules->has(path.c_str())) {
            factor *= socFloat(*rules, path.c_str(), 1.0f);
        }
    }
    return factor;
}

// Two strangers' liking for each other: the same both ways, from a hash of the pair.
float SocialSystem::compatibility(Entity a, Entity b) const {
    const uint64_t lo = std::min(socId(a), socId(b));
    const uint64_t hi = std::max(socId(a), socId(b));
    const double u = socUnit(seed_ ^ splitMix64(lo * 0x9E3779B97F4A7C15ULL ^ hi * 0xC2B2AE3D27D4EB4FULL));
    return static_cast<float>(u * 2.0 - 1.0) * rules_.compatibilityMax;
}

std::vector<OpinionLine> SocialSystem::opinionLines(Entity pawn, Entity other) const {
    std::vector<OpinionLine> lines;
    const entt::registry& reg = things_.registry();
    const Social* social = reg.valid(pawn) ? reg.try_get<Social>(pawn) : nullptr;
    if (social == nullptr || pawn == other || !reg.valid(other)) {
        return lines;
    }
    lines.push_back(OpinionLine{"compatibility", compatibility(pawn, other)});
    if (const float traits = traitOpinion(other); traits != 0.0f) {
        lines.push_back(OpinionLine{"what they are like", traits});
    }
    for (const Bond& bond : social->bonds) {
        if (bond.other == other) {
            lines.push_back(OpinionLine{bondKindName(bond.kind), rules_.bondOpinion[static_cast<int>(bond.kind)]});
        }
    }
    for (const OpinionMemory& m : social->memories) {
        if (m.other == other && m.expires > time_.seconds) {
            lines.push_back(OpinionLine{m.label, m.offset});
        }
    }
    return lines;
}

float SocialSystem::opinion(Entity pawn, Entity other) const {
    float total = 0.0f;
    for (const OpinionLine& line : opinionLines(pawn, other)) {
        total += line.offset;
    }
    return std::clamp(total, rules_.opinionMin, rules_.opinionMax);
}

bool SocialSystem::friends(Entity a, Entity b) const {
    return a != b && opinion(a, b) >= rules_.friendOpinion && opinion(b, a) >= rules_.friendOpinion;
}

bool SocialSystem::rival(Entity pawn, Entity other) const {
    return pawn != other && things_.registry().valid(pawn) && things_.registry().all_of<Social>(pawn) &&
           opinion(pawn, other) <= rules_.rivalOpinion;
}

const Bond* SocialSystem::bondBetween(Entity a, Entity b) const {
    const entt::registry& reg = things_.registry();
    const Social* social = reg.valid(a) ? reg.try_get<Social>(a) : nullptr;
    if (social == nullptr) {
        return nullptr;
    }
    for (const Bond& bond : social->bonds) {
        if (bond.other == b) {
            return &bond;
        }
    }
    return nullptr;
}

Entity SocialSystem::partner(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const Social* social = reg.valid(pawn) ? reg.try_get<Social>(pawn) : nullptr;
    if (social == nullptr) {
        return kNoEntity;
    }
    Entity lover = kNoEntity;
    for (const Bond& bond : social->bonds) {
        if (bond.kind == BondKind::Spouse && reg.valid(bond.other)) {
            return bond.other;
        }
        if (bond.kind == BondKind::Lover && reg.valid(bond.other)) {
            lover = bond.other;
        }
    }
    return lover;
}

bool SocialSystem::fighting(Entity pawn) const {
    return things_.registry().valid(pawn) && things_.registry().all_of<SocialFight>(pawn);
}

bool SocialSystem::single(Entity pawn) const {
    const Social* social = things_.registry().try_get<Social>(pawn);
    if (social == nullptr) {
        return false;
    }
    for (const Bond& bond : social->bonds) {
        if (bond.kind == BondKind::Lover || bond.kind == BondKind::Spouse) {
            return false;
        }
    }
    return true;
}

// A living colonist of the player's side that has a social life.
bool SocialSystem::takesPart(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!reg.valid(pawn) || !reg.all_of<Social>(pawn)) {
        return false;
    }
    const Health* health = reg.try_get<Health>(pawn);
    return health == nullptr || !health->dead;
}

// Awake, on its feet, on the map, and not busy fighting for the colony, under the player's hand or in a fight.
bool SocialSystem::canTalk(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    if (!takesPart(pawn) || !things_.spawned(pawn) || health_.incapacitated(pawn)) {
        return false;
    }
    if (reg.all_of<Fighter>(pawn) || reg.all_of<PlayerControl>(pawn) || reg.all_of<SocialFight>(pawn)) {
        return false;
    }
    const Worker* worker = reg.try_get<Worker>(pawn);
    return worker == nullptr || !jobIsLyingDown(worker->job);
}

// Close enough to hear, and in the same room (or both out in the open).
bool SocialSystem::inEarshot(Entity a, Entity b) {
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(a) || !things_.spawned(b)) {
        return false;
    }
    const Position& pa = reg.get<Position>(a);
    const Position& pb = reg.get<Position>(b);
    const float dx = pa.x - pb.x;
    const float dy = pa.y - pb.y;
    if (pa.level != pb.level || dx * dx + dy * dy > rules_.maxDistance * rules_.maxDistance) {
        return false;
    }
    const RoomId roomA = rooms_.roomAt(cellOf(pa));
    return roomA != kNoRoom && roomA == rooms_.roomAt(cellOf(pb));
}

// ---- Interactions ---------------------------------------------------------------------------

bool SocialSystem::eligible(Entity initiator, Entity recipient, const SocialInteraction& interaction,
                            bool checkOpinion) const {
    if (checkOpinion) {
        const float o = opinion(initiator, recipient);
        if ((interaction.hasMinOpinion && o < interaction.minOpinion) || (interaction.hasMaxOpinion && o > interaction.maxOpinion)) {
            return false;
        }
        if (interaction.bond == "Lover" || interaction.bond == "Spouse") {
            // The other one has to think as well of the asker as the asker does of them.
            if (interaction.hasMinOpinion && opinion(recipient, initiator) < interaction.minOpinion) {
                return false;
            }
        }
        if (interaction.defName == "KindWords") {
            const Mood* mood = things_.registry().try_get<Mood>(recipient);
            if (mood == nullptr || mood->level >= rules_.kindWordsMoodBelow) {
                return false;
            }
        }
    }
    if (interaction.requiresAdult && (!isAdult(initiator) || !isAdult(recipient))) {
        return false;
    }
    if (interaction.requiresSingleOrOpen && (!single(initiator) || !single(recipient))) {
        return false;
    }
    if (interaction.requiresLover) {
        const Bond* bond = bondBetween(initiator, recipient);
        if (bond == nullptr || bond->kind != BondKind::Lover) {
            return false;
        }
    }
    return true;
}

void SocialSystem::push(EventType type, Entity a, Entity b, const std::string& message, int count, float amount) {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.thing = a;
    e.target = b;
    e.message = message;
    e.count = count;
    e.amount = amount;
    events_->push_back(std::move(e));
}

void SocialSystem::thought(Entity pawn, const std::string& trigger) {
    if (!trigger.empty()) {
        mood_.remember(pawn, trigger, time_.seconds);
    }
}

void SocialSystem::learn(Entity pawn, double xp) {
    jobs_.addSkillXp(pawn, "Social", xp);
}

void SocialSystem::addMemory(Entity pawn, Entity other, const std::string& label, float offset, float days, int stackLimit) {
    Social* social = things_.registry().try_get<Social>(pawn);
    if (social == nullptr || offset == 0.0f || days <= 0.0f) {
        return;
    }
    const int64_t expires = time_.seconds + static_cast<int64_t>(std::llround(static_cast<double>(days) * static_cast<double>(kSecondsPerDay)));
    const int limit = stackLimit > 0 ? stackLimit : rules_.stackLimit;
    int same = 0;
    OpinionMemory* oldest = nullptr;
    for (OpinionMemory& m : social->memories) {
        if (m.other == other && m.label == label) {
            ++same;
            if (oldest == nullptr || m.expires < oldest->expires) {
                oldest = &m;
            }
        }
    }
    if (same >= limit && oldest != nullptr) {
        // The new one takes the oldest one's place.
        oldest->offset = offset;
        oldest->expires = expires;
        return;
    }
    social->memories.push_back(OpinionMemory{other, label, offset, expires});
}

void SocialSystem::dropBond(Entity a, Entity b, BondKind kind) {
    entt::registry& reg = things_.registry();
    for (const auto& [x, y] : {std::pair<Entity, Entity>{a, b}, std::pair<Entity, Entity>{b, a}}) {
        if (Social* social = reg.valid(x) ? reg.try_get<Social>(x) : nullptr) {
            social->bonds.erase(std::remove_if(social->bonds.begin(), social->bonds.end(),
                                               [&](const Bond& bond) { return bond.other == y && bond.kind == kind; }),
                                social->bonds.end());
        }
    }
}

// Both pawns come to hold the bond; any bond they held with each other before is replaced.
void SocialSystem::setBond(Entity a, Entity b, BondKind kind) {
    entt::registry& reg = things_.registry();
    for (const auto& [x, y] : {std::pair<Entity, Entity>{a, b}, std::pair<Entity, Entity>{b, a}}) {
        if (Social* social = reg.valid(x) ? reg.try_get<Social>(x) : nullptr) {
            social->bonds.erase(std::remove_if(social->bonds.begin(), social->bonds.end(),
                                               [&](const Bond& bond) { return bond.other == y; }),
                                social->bonds.end());
            social->bonds.push_back(Bond{kind, y, time_.seconds});
        }
    }
    push(EventType::BondChanged, a, b, bondKindName(kind), 1, 0.0f);
}

void SocialSystem::perform(Entity initiator, Entity recipient, const SocialInteraction& interaction, int outcome) {
    ++stats_.talks;
    const std::string& name = interaction.defName;

    // Romance and marriage may be turned down: the base chance, scaled by how the other one thinks of the asker.
    bool ok = true;
    if (interaction.bond == "Lover" || interaction.bond == "Spouse") {
        const float chance = std::clamp(
            interaction.successBase * (1.0f + opinion(recipient, initiator) / rules_.romanceOpinionScale), 0.0f, 0.95f);
        ok = outcome >= 0 ? outcome == 1 : unit(initiator, 7 + socId(recipient)) < static_cast<double>(chance);
    }

    if (name == "Chitchat") {
        ++stats_.chitchats;
    } else if (name == "DeepTalk") {
        ++stats_.deepTalks;
    } else if (name == "KindWords") {
        ++stats_.kindWords;
    } else if (name == "Slight") {
        ++stats_.slights;
    } else if (name == "Insult") {
        ++stats_.insults;
    } else if (interaction.bond == "Lover") {
        ++stats_.romances;
    } else if (interaction.bond == "Spouse") {
        ++stats_.proposals;
    }

    // What the listener now thinks of the talker. A turned-down romance changes nothing.
    if (ok) {
        addMemory(recipient, initiator, interaction.opinionLabel, interaction.opinionOffset, interaction.opinionDays,
                  interaction.stackLimit);
    }

    if (interaction.bond == "Lover" && ok) {
        setBond(initiator, recipient, BondKind::Lover);
        ++stats_.lovers;
    } else if (interaction.bond == "Spouse") {
        if (ok) {
            setBond(initiator, recipient, BondKind::Spouse);
            ++stats_.weddings;
            thought(initiator, interaction.acceptedTrigger);
            thought(recipient, interaction.acceptedTrigger);
        } else {
            thought(initiator, interaction.rejectedTrigger);
        }
    } else if (interaction.bond == "ExLover") {
        setBond(initiator, recipient, BondKind::ExLover);
        ++stats_.breakups;
    }
    if (interaction.bond != "Spouse") {
        thought(initiator, interaction.initiatorTrigger);
        thought(recipient, interaction.recipientTrigger);
    }

    learn(initiator, interaction.xp > 0.0f ? static_cast<double>(interaction.xp) : static_cast<double>(rules_.defaultXp));
    push(EventType::Interaction, initiator, recipient, name, ok ? 1 : 0, opinion(recipient, initiator));

    // An insult can turn into a fight (not with a wimp, nor with a kind soul, nor if one is already in one).
    if (name == "Insult") {
        const bool peaceful = hasTrait(recipient, "Wimp") || hasTrait(initiator, "Wimp") || hasTrait(recipient, "Kind");
        const float chance = rules_.fightChance * (hasTrait(recipient, "Brawler") ? 2.0f : 1.0f);
        const bool fight = !peaceful && (outcome >= 0 ? outcome == 1 : unit(recipient, 11 + socId(initiator)) < static_cast<double>(chance));
        if (fight && canTalk(initiator) && canTalk(recipient)) {
            startFight(initiator, recipient);
        }
    }
}

bool SocialSystem::interact(Entity initiator, Entity recipient, const std::string& interaction, int outcome,
                            std::string& error) {
    const SocialInteraction* def = rules_.find(interaction);
    if (def == nullptr) {
        error = "there is no interaction called " + interaction;
        return false;
    }
    if (initiator == recipient || !takesPart(initiator) || !takesPart(recipient)) {
        error = "both must be two different colonists";
        return false;
    }
    if (!canTalk(initiator) || !canTalk(recipient)) {
        error = "they can't talk now (down, asleep, fighting, drafted or not on the map)";
        return false;
    }
    if (!inEarshot(initiator, recipient)) {
        error = "they are too far apart to talk";
        return false;
    }
    if (!eligible(initiator, recipient, *def, false)) {
        error = "that doesn't fit the two of them (they need to be adults, single or lovers)";
        return false;
    }
    perform(initiator, recipient, *def, outcome);
    const int64_t gap = static_cast<int64_t>(std::llround(static_cast<double>(rules_.minGapMinutes) * 60.0));
    Social& a = things_.registry().get<Social>(initiator);
    Social& b = things_.registry().get<Social>(recipient);
    a.nextTalk = std::max(a.nextTalk, time_.seconds + gap);
    b.nextTalk = std::max(b.nextTalk, time_.seconds + gap);
    return true;
}

// ---- Fights ---------------------------------------------------------------------------------

void SocialSystem::startFight(Entity a, Entity b) {
    entt::registry& reg = things_.registry();
    if (reg.all_of<SocialFight>(a) || reg.all_of<SocialFight>(b)) {
        return;
    }
    const int64_t endsBy = time_.seconds + static_cast<int64_t>(std::llround(static_cast<double>(rules_.fightMaxMinutes) * 60.0));
    for (const auto& [x, y] : {std::pair<Entity, Entity>{a, b}, std::pair<Entity, Entity>{b, a}}) {
        jobs_.interrupt(x, "fighting");
        if (Mover* mover = reg.try_get<Mover>(x)) {
            mover->path.clear();
            mover->next = 0;
            mover->gaveUp = false;
        }
        reg.emplace<SocialFight>(x, SocialFight{y, time_.seconds, endsBy, time_.seconds});
    }
    ++stats_.fights;
    push(EventType::SocialFight, a, b, "started", 0, 0.0f);
}

void SocialSystem::endFight(Entity a, Entity b, const char* why) {
    entt::registry& reg = things_.registry();
    const int64_t gap = static_cast<int64_t>(std::llround(static_cast<double>(rules_.minGapMinutes) * 60.0));
    for (const Entity x : {a, b}) {
        if (x != kNoEntity && reg.valid(x) && reg.all_of<SocialFight>(x)) {
            reg.remove<SocialFight>(x);
            if (Mover* mover = reg.try_get<Mover>(x)) {
                mover->path.clear();
                mover->next = 0;
            }
            if (Social* social = reg.try_get<Social>(x)) {
                social->nextTalk = std::max(social->nextTalk, time_.seconds + gap);
            }
        }
    }
    // Each thinks worse of the other for a while.
    if (b != kNoEntity && reg.valid(a) && reg.valid(b)) {
        addMemory(a, b, rules_.fightLabel, rules_.fightOffset, rules_.fightDays, 1);
        addMemory(b, a, rules_.fightLabel, rules_.fightOffset, rules_.fightDays, 1);
    }
    push(EventType::SocialFight, a, b, std::string("ended: ") + why, 0, 0.0f);
}

void SocialSystem::stepFights() {
    entt::registry& reg = things_.registry();
    std::vector<Entity> fighters(reg.view<SocialFight>().begin(), reg.view<SocialFight>().end());
    if (fighters.empty()) {
        return;
    }
    std::sort(fighters.begin(), fighters.end());
    const int64_t now = time_.seconds;
    for (const Entity pawn : fighters) {
        if (!reg.valid(pawn) || !reg.all_of<SocialFight>(pawn)) {
            continue;  // its fight ended a moment ago, with the other one's
        }
        const SocialFight f = reg.get<SocialFight>(pawn);
        const Entity opp = f.opponent;
        const char* why = nullptr;
        if (!reg.valid(opp) || !reg.all_of<SocialFight>(opp) || reg.get<SocialFight>(opp).opponent != pawn) {
            why = "the other one is gone";
        } else if (!things_.spawned(pawn) || !things_.spawned(opp)) {
            why = "one left the map";
        } else if (health_.incapacitated(pawn) || health_.incapacitated(opp)) {
            why = "someone went down";
        } else if (reg.all_of<Fighter>(pawn) || reg.all_of<Fighter>(opp) || reg.all_of<PlayerControl>(pawn) ||
                   reg.all_of<PlayerControl>(opp)) {
            why = "called away";
        } else if (now >= f.endsBy) {
            why = "they tired of it";
        } else {
            for (const Entity x : {pawn, opp}) {
                const Health* health = reg.try_get<Health>(x);
                if (health != nullptr && (health->pain >= rules_.fightPain || health->bloodLoss >= rules_.fightBloodLoss)) {
                    why = "someone was hurt";
                }
            }
        }
        if (why != nullptr) {
            endFight(pawn, opp, why);
            continue;
        }
        if (melee_.inReach(pawn, opp, true)) {
            if (Mover* mover = reg.try_get<Mover>(pawn)) {
                mover->path.clear();
                mover->next = 0;
            }
            if (melee_.ready(pawn)) {
                melee_.attack(pawn, opp, false, nullptr, true);
            }
        } else if (now >= f.nextMove) {
            const Position& from = reg.get<Position>(pawn);
            const Position& to = reg.get<Position>(opp);
            reg.get<Mover>(pawn).walkTo(from.x, from.y, std::vector<CellPos>{cellOf(to)});
            reg.get<SocialFight>(pawn).nextMove = now + static_cast<int64_t>(rules_.fightWalkEvery);
        }
    }
}

// ---- Deaths ---------------------------------------------------------------------------------

// Those who cared about a pawn that died think about it: the strongest tie decides which thought.
void SocialSystem::mourn(Entity dead) {
    entt::registry& reg = things_.registry();
    std::vector<Entity> others(reg.view<Social>().begin(), reg.view<Social>().end());
    std::sort(others.begin(), others.end());
    for (const Entity pawn : others) {
        if (pawn == dead || !takesPart(pawn)) {
            continue;
        }
        std::string trigger;
        if (const Bond* bond = bondBetween(pawn, dead)) {
            if (bond->kind == BondKind::Spouse) {
                trigger = rules_.spouseDeath;
            } else if (bond->kind == BondKind::Lover) {
                trigger = rules_.loverDeath;
            }
        }
        if (trigger.empty() && friends(pawn, dead)) {
            trigger = rules_.friendDeath;
        }
        if (trigger.empty() && rival(pawn, dead)) {
            trigger = rules_.rivalDeath;
        }
        thought(pawn, trigger);
        // A widow or a bereaved lover is free to love again.
        dropBond(pawn, dead, BondKind::Lover);
        dropBond(pawn, dead, BondKind::Spouse);
    }
}

// ---- The step -------------------------------------------------------------------------------

void SocialSystem::roll(Entity pawn, const std::vector<Entity>& pawns) {
    entt::registry& reg = things_.registry();
    // Playing or chatting beside others: they talk to each other more.
    Entity group = kNoEntity;
    if (const Worker* worker = reg.try_get<Worker>(pawn)) {
        if (worker->job.type == JobType::Recreate && worker->job.step > 0 && worker->job.thing != kNoEntity) {
            group = worker->job.thing;
        }
    }
    const double meanSeconds = group != kNoEntity ? static_cast<double>(rules_.tableChatMeanMinutes) * 60.0
                                                  : static_cast<double>(rules_.meanHours) * static_cast<double>(kSecondsPerHour);
    if (unit(pawn, 1) >= 1.0 / meanSeconds) {
        return;
    }

    candidates_.clear();
    for (const Entity other : pawns) {
        if (other == pawn || !inEarshot(pawn, other)) {
            continue;
        }
        if (group != kNoEntity) {
            const Worker* worker = reg.try_get<Worker>(other);
            if (worker == nullptr || worker->job.type != JobType::Recreate || worker->job.thing != group) {
                continue;
            }
        }
        for (size_t i = 0; i < rules_.interactions.size(); ++i) {
            const SocialInteraction& interaction = rules_.interactions[i];
            if (!eligible(pawn, other, interaction, true)) {
                continue;
            }
            const float weight = interaction.weight * traitWeight(pawn, interaction.defName);
            if (weight > 0.0f) {
                candidates_.push_back(Candidate{other, i, weight});
            }
        }
    }
    if (candidates_.empty()) {
        return;
    }
    float total = 0.0f;
    for (const Candidate& c : candidates_) {
        total += c.weight;
    }
    float pick = static_cast<float>(unit(pawn, 2)) * total;
    const Candidate* chosen = &candidates_.back();
    for (const Candidate& c : candidates_) {
        pick -= c.weight;
        if (pick < 0.0f) {
            chosen = &c;
            break;
        }
    }
    const Entity recipient = chosen->other;
    perform(pawn, recipient, rules_.interactions[chosen->interaction], -1);
    const int64_t gap = static_cast<int64_t>(std::llround(static_cast<double>(rules_.minGapMinutes) * 60.0));
    reg.get<Social>(pawn).nextTalk = time_.seconds + gap;
    if (reg.valid(recipient) && reg.all_of<Social>(recipient)) {
        reg.get<Social>(recipient).nextTalk = time_.seconds + gap;
    }
}

void SocialSystem::step(int64_t now) {
    if (!rules_.present) {
        return;
    }
    entt::registry& reg = things_.registry();

    // New colonists get a social life.
    std::vector<Entity> made;
    for (const Entity pawn : reg.view<Needs>()) {
        const Faction* faction = reg.try_get<Faction>(pawn);
        if (!reg.all_of<Social>(pawn) && !reg.all_of<Animal>(pawn) && (faction == nullptr || faction->id == kPlayerFaction)) {
            made.push_back(pawn);
        }
    }
    for (const Entity pawn : made) {
        reg.emplace<Social>(pawn);
    }

    std::vector<Entity> pawns(reg.view<Social>().begin(), reg.view<Social>().end());
    std::sort(pawns.begin(), pawns.end());

    // A death is mourned once.
    for (const Entity pawn : pawns) {
        Social& social = reg.get<Social>(pawn);
        const Health* health = reg.try_get<Health>(pawn);
        if (!social.mourned && health != nullptr && health->dead) {
            social.mourned = true;
            mourn(pawn);
        }
    }

    // Opinions fade; memories and bonds about pawns that no longer exist go.
    for (const Entity pawn : pawns) {
        Social& social = reg.get<Social>(pawn);
        social.memories.erase(std::remove_if(social.memories.begin(), social.memories.end(),
                                             [&](const OpinionMemory& m) { return m.expires <= now || !reg.valid(m.other); }),
                              social.memories.end());
        social.bonds.erase(std::remove_if(social.bonds.begin(), social.bonds.end(),
                                          [&](const Bond& b) { return !reg.valid(b.other); }),
                           social.bonds.end());
    }

    stepFights();

    std::vector<Entity> talkers;
    for (const Entity pawn : pawns) {
        if (canTalk(pawn)) {
            talkers.push_back(pawn);
        }
    }
    if (talkers.size() < 2) {
        return;
    }
    for (const Entity pawn : talkers) {
        if (reg.get<Social>(pawn).nextTalk <= now && canTalk(pawn)) {
            roll(pawn, talkers);
        }
    }
}

// ---- Hash -----------------------------------------------------------------------------------

void SocialSystem::hashThing(Entity e, StateHasher& h) const {
    const entt::registry& reg = things_.registry();
    if (const Social* social = reg.try_get<Social>(e)) {
        h.add(social->nextTalk);
        h.add(social->mourned);
        h.add(static_cast<uint64_t>(social->memories.size()));
        for (const OpinionMemory& m : social->memories) {
            h.add(socId(m.other));
            h.add(m.label.data(), m.label.size());
            h.add(m.offset);
            h.add(m.expires);
        }
        h.add(static_cast<uint64_t>(social->bonds.size()));
        for (const Bond& b : social->bonds) {
            h.add(static_cast<int>(b.kind));
            h.add(socId(b.other));
            h.add(b.since);
        }
    }
    if (const SocialFight* fight = reg.try_get<SocialFight>(e)) {
        h.add(socId(fight->opponent));
        h.add(fight->startedAt);
        h.add(fight->endsBy);
        h.add(fight->nextMove);
    }
}

}  // namespace colony

// Quests (task Q1, docs/design/quests.md): offers, answers, the four kinds that need no world travel, rewards.
#include "ColonySim/Quests.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Map.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <set>

namespace colony {

namespace {

// The four kinds a Quests def can have (its "questType").
constexpr const char* kQuestRefugees = "Refugees";
constexpr const char* kQuestWanderer = "Wanderer";
constexpr const char* kQuestStash = "Stash";
constexpr const char* kQuestDefend = "Defend";

// A number between the def's two-number range `path` (inclusive), or `fallback` if it has none.
int questRollInt(const Def& def, const char* path, Rng& rng, int fallback) {
    const std::string base(path);
    if (def.count(path) < 2) {
        return fallback;
    }
    const int low = static_cast<int>(def.number((base + ".0").c_str(), fallback));
    const int high = std::max(low, static_cast<int>(def.number((base + ".1").c_str(), fallback)));
    return low + static_cast<int>(rng.below(static_cast<uint32_t>(high - low + 1)));
}

float questRollFloat(const Def& def, const char* path, Rng& rng, float fallback) {
    const std::string base(path);
    if (def.count(path) < 2) {
        return fallback;
    }
    const float low = static_cast<float>(def.number((base + ".0").c_str(), fallback));
    const float high = std::max(low, static_cast<float>(def.number((base + ".1").c_str(), fallback)));
    return low + rng.unit() * (high - low);
}

bool questIsOpen(QuestState state) {
    return state == QuestState::Offered || state == QuestState::Active;
}

}  // namespace

const char* questStateName(QuestState state) {
    switch (state) {
        case QuestState::Offered: return "Offered";
        case QuestState::Active: return "Active";
        case QuestState::Completed: return "Completed";
        case QuestState::Failed: return "Failed";
        case QuestState::Declined: return "Declined";
        case QuestState::Expired: return "Expired";
    }
    return "?";
}

QuestRules QuestRules::read(const DefDatabase& defs) {
    QuestRules r;
    const Def* rules = defs.find("Rules", "QuestRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    const auto num = [&](const char* key, int& out) { out = static_cast<int>(rules->integer(key, out)); };
    num("maxOffered", r.maxOffered);
    num("maxActive", r.maxActive);
    num("historyKept", r.historyKept);
    num("rewardSpread", r.rewardSpread);
    num("deliverySearch", r.deliverySearch);
    num("edgeDepth", r.edgeDepth);
    r.maxOffered = std::max(1, r.maxOffered);
    r.maxActive = std::max(1, r.maxActive);
    r.historyKept = std::max(0, r.historyKept);
    r.edgeDepth = std::max(0, r.edgeDepth);
    return r;
}

QuestSystem::QuestSystem(Things& things, Map& map, RaidSystem& raids, FighterSystem& fighters, HealthSystem& health,
                         const DefDatabase& defs, const GameTime& time, Rng& rng)
    : things_(things), map_(map), raids_(raids), fighters_(fighters), health_(health), defs_(defs), time_(time), rng_(rng) {
    rules_ = QuestRules::read(defs_);
}

int64_t QuestSystem::now() const {
    return time_.seconds;
}

const Quest* QuestSystem::find(uint32_t id) const {
    for (const Quest& q : state_.quests) {
        if (q.id == id) {
            return &q;
        }
    }
    return nullptr;
}

Quest* QuestSystem::findMutable(uint32_t id) {
    return const_cast<Quest*>(find(id));
}

int QuestSystem::count(QuestState state) const {
    return static_cast<int>(std::count_if(state_.quests.begin(), state_.quests.end(), [&](const Quest& q) { return q.state == state; }));
}

void QuestSystem::emit(EventType type, const Quest& quest, const char* message) const {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.count = static_cast<int>(quest.id);
    e.squad = quest.squad;
    e.def = defs_.idOf("Quests", quest.kind);
    e.message = message != nullptr && message[0] != '\0' ? message : quest.kind;
    events_->push_back(std::move(e));
}

void QuestSystem::finish(Quest& quest, QuestState state, const char* message) {
    quest.state = state;
    switch (state) {
        case QuestState::Completed: emit(EventType::QuestCompleted, quest, message); break;
        case QuestState::Failed: emit(EventType::QuestFailed, quest, message); break;
        case QuestState::Declined: emit(EventType::QuestDeclined, quest, message); break;
        default: emit(EventType::QuestExpired, quest, message); break;
    }
}

void QuestSystem::trim() {
    // The finished quests beyond the newest `historyKept` are dropped (the list stays in id order).
    int finished = 0;
    for (const Quest& q : state_.quests) {
        finished += questIsOpen(q.state) ? 0 : 1;
    }
    for (auto it = state_.quests.begin(); it != state_.quests.end() && finished > rules_.historyKept;) {
        if (!questIsOpen(it->state)) {
            it = state_.quests.erase(it);
            --finished;
        } else {
            ++it;
        }
    }
}

bool QuestSystem::alive(Entity pawn) const {
    if (!things_.exists(pawn)) {
        return false;
    }
    const Health* health = things_.registry().try_get<Health>(pawn);
    return health != nullptr && !health->dead;
}

CellPos QuestSystem::colonyCentre() const {
    const entt::registry& reg = things_.registry();
    double sx = 0.0;
    double sy = 0.0;
    int n = 0;
    for (const auto [pawn, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
        if (fighters_.factionOf(pawn) == kPlayerFaction && alive(pawn)) {
            sx += pos.x;
            sy += pos.y;
            ++n;
        }
    }
    if (n == 0) {
        return CellPos{map_.width() / 2, map_.height() / 2, 0};
    }
    return CellPos{static_cast<int>(sx / n), static_cast<int>(sy / n), 0};
}

bool QuestSystem::nearbyCells(CellPos around, size_t wanted, std::vector<CellPos>& out) const {
    out.clear();
    CellPos start = around;
    start.level = 0;
    if (!map_.inBounds(start)) {
        return false;
    }
    std::vector<CellPos> open{start};
    std::set<int> seen{map_.index(start)};
    for (size_t head = 0; head < open.size() && out.size() < wanted && static_cast<int>(open.size()) < rules_.deliverySearch; ++head) {
        const CellPos c = open[head];
        if (map_.walkable(c) && map_.obstacleDm(c) == 0 && !map_.isDoor(c)) {
            out.push_back(c);
        }
        const CellPos next[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
        for (const CellPos& n : next) {
            if (map_.inBounds(n) && seen.insert(map_.index(n)).second) {
                open.push_back(n);
            }
        }
    }
    return out.size() >= wanted;
}

bool QuestSystem::edgeCells(size_t wanted, std::vector<CellPos>& out) const {
    // Cells just inside a map edge that can reach the colony; the people arrive together, so the cells come from one spot.
    out.clear();
    const int w = map_.width();
    const int h = map_.height();
    const CellPos centre = colonyCentre();
    Regions& regions = map_.regions();
    const int d = rules_.edgeDepth;
    const int firstSide = static_cast<int>(rng_.below(4));
    for (int attempt = 0; attempt < 160; ++attempt) {
        const int side = (firstSide + attempt / 40) % 4;
        const int along = static_cast<int>(rng_.below(static_cast<uint32_t>(side % 2 == 0 ? w : h)));
        const CellPos guess = side == 0 ? CellPos{along, h - 1 - d, 0}
                            : side == 1 ? CellPos{w - 1 - d, along, 0}
                            : side == 2 ? CellPos{along, d, 0}
                                        : CellPos{d, along, 0};
        std::vector<CellPos> cells;
        if (map_.walkable(guess) && regions.canReach(guess, centre) && nearbyCells(guess, wanted, cells)) {
            out = std::move(cells);
            return true;
        }
    }
    return false;
}

std::vector<std::string> QuestSystem::available() const {
    std::vector<std::string> kinds;
    if (count(QuestState::Offered) >= rules_.maxOffered) {
        return kinds;
    }
    int colonists = 0;
    for (const auto [pawn, colonist] : things_.registry().view<const Colonist>().each()) {
        colonists += fighters_.factionOf(pawn) == kPlayerFaction && alive(pawn) ? 1 : 0;
    }
    for (const DefId id : defs_.idsOfKind("Quests")) {
        const Def& def = defs_.get(id);
        if (time_.day() < def.integer("earliestDay", 0) || colonists < def.integer("minColonists", 1)) {
            continue;
        }
        const bool taken = std::any_of(state_.quests.begin(), state_.quests.end(),
                                       [&](const Quest& q) { return q.kind == def.defName() && questIsOpen(q.state); });
        if (!taken) {
            kinds.push_back(def.defName());
        }
    }
    return kinds;
}

bool QuestSystem::offer(const std::string& kind, uint32_t& id, std::string& error) {
    id = 0;
    const Def* def = defs_.find("Quests", kind);
    if (def == nullptr) {
        error = "there is no quest '" + kind + "'";
        return false;
    }
    const std::vector<std::string> kinds = available();
    if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) {
        error = count(QuestState::Offered) >= rules_.maxOffered ? "too many quests are waiting for an answer"
                                                                : "the quest '" + kind + "' can't be offered now";
        return false;
    }
    Quest q;
    q.id = state_.next++;
    q.kind = kind;
    q.offeredAt = now();
    q.expiresAt = now() + static_cast<int64_t>(questRollFloat(*def, "offerDays", rng_, 3.0f) * static_cast<float>(kSecondsPerDay));
    const std::string type = def->text("questType");
    if (type == kQuestRefugees || type == kQuestDefend) {
        q.faction = def->text("faction", "Pirate");
        q.points = questRollFloat(*def, "points", rng_, 200.0f);
    }
    q.count = type == kQuestRefugees ? questRollInt(*def, "people", rng_, 2) : type == kQuestWanderer ? 1 : 0;
    for (size_t i = 0; i < def->count("rewards"); ++i) {
        const std::string base = "rewards." + std::to_string(i);
        Quest::Reward r;
        r.thing = def->text((base + ".thing").c_str());
        r.count = questRollInt(*def, (base + ".count").c_str(), rng_, 1);
        if (r.count > 0 && defs_.idOf("Things", r.thing).valid()) {
            q.rewards.push_back(std::move(r));
        }
    }
    id = q.id;
    state_.quests.push_back(std::move(q));
    emit(EventType::QuestOffered, state_.quests.back(), nullptr);
    trim();
    return true;
}

bool QuestSystem::offerRandom(uint32_t& id, std::string& error) {
    id = 0;
    const std::vector<std::string> kinds = available();
    if (kinds.empty()) {
        error = "no quest can be offered now";
        return false;
    }
    float total = 0.0f;
    std::vector<float> weights;
    for (const std::string& kind : kinds) {
        weights.push_back(static_cast<float>(std::max(0.0, defs_.find("Quests", kind)->number("weight", 1.0))));
        total += weights.back();
    }
    if (total <= 0.0f) {
        error = "no quest can be offered now";
        return false;
    }
    float pick = rng_.unit() * total;
    size_t chosen = kinds.size() - 1;
    for (size_t i = 0; i < kinds.size(); ++i) {
        if (pick < weights[i]) {
            chosen = i;
            break;
        }
        pick -= weights[i];
    }
    return offer(kinds[chosen], id, error);
}

bool QuestSystem::peopleArrive(Quest& quest, int count) {
    if (!joinMaker_) {
        return false;
    }
    std::vector<CellPos> cells;
    if (!edgeCells(static_cast<size_t>(count) * 2, cells)) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const uint64_t seed = (static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next();
        const Entity pawn = joinMaker_(cells[static_cast<size_t>(i) * 2], seed);
        if (pawn != kNoEntity) {
            quest.people.push_back(pawn);
        }
    }
    return !quest.people.empty();
}

bool QuestSystem::accept(uint32_t id, std::string& error) {
    Quest* quest = findMutable(id);
    if (quest == nullptr || quest->state != QuestState::Offered) {
        error = "that quest is not waiting for an answer";
        return false;
    }
    if (count(QuestState::Active) >= rules_.maxActive) {
        error = "too many quests are under way";
        return false;
    }
    const Def* def = defs_.find("Quests", quest->kind);
    if (def == nullptr) {
        error = "the quest's def is gone";
        return false;
    }
    const std::string type = def->text("questType");
    if (type == kQuestRefugees || type == kQuestWanderer) {
        if (!peopleArrive(*quest, quest->count)) {
            error = "nobody could reach the colony";
            return false;
        }
    } else if (type == kQuestStash) {
        if (!pay(*quest)) {
            error = "there is no room to deliver the goods";
            return false;
        }
    }
    quest->state = QuestState::Active;
    quest->acceptedAt = now();
    emit(EventType::QuestAccepted, *quest, nullptr);
    if (type == kQuestRefugees || type == kQuestDefend) {
        quest->launchAt = now() + static_cast<int64_t>(questRollFloat(*def, "delayHours", rng_, 2.0f) * static_cast<float>(kSecondsPerHour));
        quest->deadline = quest->launchAt + static_cast<int64_t>(def->number("deadlineHours", 24.0) * kSecondsPerHour);
    }
    if (type == kQuestWanderer) {
        pay(*quest);
        finish(*quest, QuestState::Completed, "joined");
    } else if (type == kQuestStash) {
        finish(*quest, QuestState::Completed, "delivered");
    }
    return true;
}

bool QuestSystem::decline(uint32_t id, std::string& error) {
    Quest* quest = findMutable(id);
    if (quest == nullptr || quest->state != QuestState::Offered) {
        error = "that quest is not waiting for an answer";
        return false;
    }
    finish(*quest, QuestState::Declined, nullptr);
    trim();
    return true;
}

bool QuestSystem::pay(const Quest& quest) {
    size_t cellsNeeded = 0;
    std::vector<std::pair<DefId, int>> stacks;
    for (const Quest::Reward& r : quest.rewards) {
        const DefId item = defs_.idOf("Things", r.thing);
        if (!item.valid()) {
            continue;
        }
        const int limit = std::max(1, static_cast<int>(defs_.get(item).integer("stackLimit", 1)));
        for (int left = r.count; left > 0; left -= limit) {
            stacks.emplace_back(item, std::min(left, limit));
            ++cellsNeeded;
        }
    }
    if (stacks.empty()) {
        return true;
    }
    std::vector<CellPos> cells;
    // Round the colony's middle, a few metres out so the goods are not under anyone's feet.
    CellPos at = colonyCentre();
    at.x = std::min(map_.width() - 1, at.x + rules_.rewardSpread / 2);
    if (!nearbyCells(at, cellsNeeded, cells)) {
        return false;
    }
    for (size_t i = 0; i < stacks.size(); ++i) {
        things_.spawn(stacks[i].first, cells[i], stacks[i].second);
    }
    return true;
}

bool QuestSystem::launchRaid(Quest& quest) {
    std::string error;
    const size_t before = raids_.state().squads.size();
    quest.launched = true;
    if (!raids_.startRaid(quest.faction, quest.points, -1, error) || raids_.state().squads.size() <= before) {
        return false;
    }
    quest.squad = raids_.state().squads.back().id;
    return true;
}

void QuestSystem::step() {
    // Once a game second is plenty: every deadline here is minutes long at least.
    for (Quest& quest : state_.quests) {
        if (quest.state == QuestState::Offered) {
            if (now() >= quest.expiresAt) {
                finish(quest, QuestState::Expired, nullptr);
            }
            continue;
        }
        if (quest.state != QuestState::Active) {
            continue;
        }
        const Def* def = defs_.find("Quests", quest.kind);
        const std::string type = def != nullptr ? def->text("questType") : std::string();
        if (type == kQuestRefugees || type == kQuestDefend) {
            if (type == kQuestRefugees && !quest.people.empty() &&
                std::none_of(quest.people.begin(), quest.people.end(), [&](Entity p) { return alive(p); })) {
                finish(quest, QuestState::Failed, "the refugees died");
                continue;
            }
            if (!quest.launched && now() >= quest.launchAt) {
                if (!launchRaid(quest)) {
                    // Nobody could be sent: the people are the colony's, and the quest ends without a fight.
                    finish(quest, type == kQuestRefugees ? QuestState::Completed : QuestState::Expired, "no raid came");
                    if (quest.state == QuestState::Completed) {
                        pay(quest);
                    }
                }
                continue;
            }
            if (quest.launched) {
                const auto& squads = raids_.state().squads;
                const bool over = std::none_of(squads.begin(), squads.end(), [&](const Squad& s) { return s.id == quest.squad; });
                if (over) {
                    pay(quest);
                    finish(quest, QuestState::Completed, "the raid is over");
                } else if (now() >= quest.deadline) {
                    finish(quest, QuestState::Failed, "the raid went on too long");
                }
            }
        }
    }
    trim();
}

void QuestSystem::hashState(StateHasher& h) const {
    h.add(state_.next);
    h.add(static_cast<uint64_t>(state_.quests.size()));
    for (const Quest& q : state_.quests) {
        h.add(q.id);
        h.add(q.kind.data(), q.kind.size());
        h.add(static_cast<int>(q.state));
        h.add(q.offeredAt);
        h.add(q.expiresAt);
        h.add(q.acceptedAt);
        h.add(q.launchAt);
        h.add(q.deadline);
        h.add(q.launched);
        h.add(q.count);
        h.add(q.points);
        h.add(q.squad);
        h.add(static_cast<uint64_t>(q.people.size()));
        for (const Entity p : q.people) {
            h.add(static_cast<uint32_t>(entt::to_integral(p)));
        }
        for (const Quest::Reward& r : q.rewards) {
            h.add(r.thing.data(), r.thing.size());
            h.add(r.count);
        }
    }
}

}  // namespace colony

#include "ColonySim/Trade.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/Hash.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Random.h"
#include "ColonySim/Regions.h"
#include "ColonySim/Talents.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace colony {

namespace {

// Names are prefixed because Unreal's unity builds merge .cpp files together.

bool tradeCellLess(const CellPos& a, const CellPos& b) {
    return a.level != b.level ? a.level < b.level : a.y != b.y ? a.y < b.y : a.x < b.x;
}

float tradeCurve(const std::vector<std::pair<float, float>>& points, float x) {
    if (points.empty()) {
        return 0.0f;
    }
    if (x <= points.front().first) {
        return points.front().second;
    }
    for (size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].first) {
            const float span = points[i].first - points[i - 1].first;
            const float t = span > 0.0f ? (x - points[i - 1].first) / span : 1.0f;
            return points[i - 1].second + (points[i].second - points[i - 1].second) * t;
        }
    }
    return points.back().second;
}

constexpr int kEdgeMargin = 3;

float tradeCentre(int v) {
    return static_cast<float>(v) + 0.5f;
}

uint64_t tradeNameHash(const std::string& name) {
    uint64_t h = 0xCBF29CE484222325ULL;
    for (const char c : name) {
        h = (h ^ static_cast<unsigned char>(c)) * 0x100000001B3ULL;
    }
    return h;
}

// Adds `count` of `thing` to a stock sorted by name.
void tradeAddStock(std::vector<TradeLine>& stock, const std::string& thing, int count) {
    auto it = std::lower_bound(stock.begin(), stock.end(), thing, [](const TradeLine& l, const std::string& n) { return l.thing < n; });
    if (it != stock.end() && it->thing == thing) {
        it->count += count;
    } else {
        stock.insert(it, TradeLine{thing, count});
    }
}

bool tradeHas(const std::vector<std::string>& list, const std::string& word) {
    return std::find(list.begin(), list.end(), word) != list.end();
}

}  // namespace

const char* visitKindName(VisitKind kind) {
    switch (kind) {
        case VisitKind::Visitors: return "Visitors";
        case VisitKind::Caravan: return "Caravan";
        case VisitKind::Orbital: return "Orbital";
        case VisitKind::Settlement: return "Settlement";
    }
    return "?";
}

bool visitKindFromName(const std::string& name, VisitKind& out) {
    for (const VisitKind kind : {VisitKind::Visitors, VisitKind::Caravan, VisitKind::Orbital, VisitKind::Settlement}) {
        if (name == visitKindName(kind)) {
            out = kind;
            return true;
        }
    }
    return false;
}

const char* visitPhaseName(VisitPhase phase) {
    switch (phase) {
        case VisitPhase::Staying: return "Staying";
        case VisitPhase::Leaving: return "Leaving";
        case VisitPhase::Defending: return "Defending";
    }
    return "?";
}

TradeRules TradeRules::read(const DefDatabase& defs) {
    TradeRules r;
    if (const Def* faction = defs.find("Rules", "FactionRules")) {
        r.hostileAtOrBelow = static_cast<float>(faction->number("hostileAtOrBelow", r.hostileAtOrBelow));
        r.alliedAtOrAbove = static_cast<float>(faction->number("alliedAtOrAbove", r.alliedAtOrAbove));
        r.goodwillMin = static_cast<float>(faction->number("goodwillRange.0", r.goodwillMin));
        r.goodwillMax = static_cast<float>(faction->number("goodwillRange.1", r.goodwillMax));
        r.driftPerDay = static_cast<float>(faction->number("driftTowardNaturalPerDay", r.driftPerDay));
        r.releaseGoodwill = static_cast<float>(faction->number("goodwillChanges.releasePrisoner", r.releaseGoodwill));
        r.giftSilverPerPoint = std::max(1.0f, static_cast<float>(faction->number("goodwillChanges.giftSilverPerPoint", r.giftSilverPerPoint)));
        r.visitorKilledGoodwill = static_cast<float>(faction->number("goodwillChanges.visitorKilled", r.visitorKilledGoodwill));
        r.executedGoodwill = static_cast<float>(faction->number("goodwillChanges.prisonerExecuted", r.executedGoodwill));
    }
    const Def* rules = defs.find("Rules", "TradeRules");
    if (rules == nullptr) {
        return r;
    }
    r.present = true;
    const auto num = [&](const char* key, float& out) { out = static_cast<float>(rules->number(key, out)); };
    num("colonySellsAt", r.colonySellsAt);
    num("colonyBuysAt", r.colonyBuysAt);
    const std::vector<std::string> categories = rules->keys("sellCategoryFactors");
    if (!categories.empty()) {
        r.sellCategoryFactors.clear();
        for (const std::string& name : categories) {
            r.sellCategoryFactors[name] = static_cast<float>(rules->number(("sellCategoryFactors." + name).c_str(), 1.0));
        }
    }
    if (const size_t n = rules->count("alwaysAccepted")) {
        r.alwaysAccepted.clear();
        for (size_t i = 0; i < n; ++i) {
            r.alwaysAccepted.push_back(rules->textAt("alwaysAccepted", i));
        }
    }
    if (const size_t n = rules->count("priceImprovementBySocial")) {
        r.improvementBySocial.clear();
        for (size_t i = 0; i < n; ++i) {
            const std::string at = "priceImprovementBySocial." + std::to_string(i);
            r.improvementBySocial.emplace_back(static_cast<float>(rules->number((at + ".0").c_str())),
                                               static_cast<float>(rules->number((at + ".1").c_str())));
        }
        std::sort(r.improvementBySocial.begin(), r.improvementBySocial.end());
    }
    num("improvementCap", r.improvementCap);
    num("improvementFloor", r.improvementFloor);
    num("orbitalStayHours", r.orbitalStayHours);
    num("caravanStayHours.0", r.caravanStayHoursMin);
    num("caravanStayHours.1", r.caravanStayHoursMax);
    num("visitorStayHours.0", r.visitorStayHoursMin);
    num("visitorStayHours.1", r.visitorStayHoursMax);
    r.visitorCountMin = static_cast<int>(rules->integer("visitorCount.0", r.visitorCountMin));
    r.visitorCountMax = static_cast<int>(rules->integer("visitorCount.1", r.visitorCountMax));
    r.guardCountMin = static_cast<int>(rules->integer("guardCount.0", r.guardCountMin));
    r.guardCountMax = static_cast<int>(rules->integer("guardCount.1", r.guardCountMax));
    num("wanderRadius", r.wanderRadius);
    r.wanderEverySeconds = std::max<int>(1, static_cast<int>(rules->integer("wanderEverySeconds", r.wanderEverySeconds)));
    r.edgeDepth = static_cast<int>(rules->integer("edgeDepth", r.edgeDepth));
    r.leaveTimeoutSeconds = static_cast<int>(rules->integer("leaveTimeoutSeconds", r.leaveTimeoutSeconds));
    r.dropRadius = static_cast<int>(rules->integer("dropRadius", r.dropRadius));
    num("settlementDiscount", r.settlementDiscount);
    num("settlementRestockDays", r.settlementRestockDays);
    r.settlementRestockDays = std::max(0.1f, r.settlementRestockDays);
    num("comms.requestGoodwillCost", r.requestGoodwillCost);
    num("comms.requestCooldownDays", r.requestCooldownDays);
    num("comms.requestArrivalDays", r.requestArrivalDays);
    num("comms.beaconRadius", r.beaconRadius);
    r.visitorCountMax = std::max(r.visitorCountMax, r.visitorCountMin);
    r.guardCountMax = std::max(r.guardCountMax, r.guardCountMin);
    r.caravanStayHoursMax = std::max(r.caravanStayHoursMax, r.caravanStayHoursMin);
    r.visitorStayHoursMax = std::max(r.visitorStayHoursMax, r.visitorStayHoursMin);
    return r;
}

TradeSystem::TradeSystem(Things& things, Map& map, CombatSystem& combat, FighterSystem& fighters, HealthSystem& health,
                         JobSystem& jobs, RaidSystem& raids, const DefDatabase& defs, const GameTime& time, Rng& rng, uint64_t seed)
    : things_(things),
      map_(map),
      combat_(combat),
      fighters_(fighters),
      health_(health),
      jobs_(jobs),
      raids_(raids),
      defs_(defs),
      time_(time),
      rng_(rng),
      seed_(seed) {
    rules_ = TradeRules::read(defs_);
}

int64_t TradeSystem::now() const {
    return time_.seconds;
}

void TradeSystem::emit(EventType type, uint32_t visit, Entity thing, float amount, const std::string& message) const {
    if (events_ == nullptr) {
        return;
    }
    Event e;
    e.type = type;
    e.squad = visit;
    e.thing = thing;
    e.amount = amount;
    e.message = message;
    const entt::registry& reg = things_.registry();
    if (reg.valid(thing)) {
        if (const Thing* t = reg.try_get<Thing>(thing)) {
            e.def = t->def;
        }
        if (const Position* p = reg.try_get<Position>(thing)) {
            e.cell = cellOf(*p);
        }
    }
    events_->push_back(std::move(e));
}

// ---- Goodwill ---------------------------------------------------------------------------------

float TradeSystem::startGoodwill(const std::string& faction) const {
    const Def* def = defs_.find("Factions", faction);
    if (def == nullptr) {
        return 0.0f;
    }
    const float lo = static_cast<float>(def->number("startingGoodwill.0", 0.0));
    const float hi = static_cast<float>(def->number("startingGoodwill.1", lo));
    if (hi <= lo) {
        return lo;
    }
    // From the world seed and the faction's name, so no random number is drawn and every faction keeps its start in a save.
    const float unit = static_cast<float>(splitMix64(seed_ ^ tradeNameHash(faction)) >> 40) / static_cast<float>(1 << 24);
    return std::round(lo + (hi - lo) * unit);
}

float TradeSystem::goodwill(const std::string& faction) const {
    if (owner_ != nullptr) {
        return owner_->goodwill(faction);
    }
    for (const Standing& s : state_.standings) {
        if (s.faction == faction) {
            return s.goodwill;
        }
    }
    return startGoodwill(faction);
}

bool TradeSystem::adjustGoodwill(const std::string& faction, float delta, std::string& error) {
    if (owner_ != nullptr) {
        return owner_->adjustGoodwill(faction, delta, error);  // this map's step turns its settlement hostile if need be
    }
    const Def* def = defs_.find("Factions", faction);
    if (def == nullptr) {
        error = "no Factions def '" + faction + "'";
        return false;
    }
    if (!std::isfinite(delta)) {
        error = "the goodwill change must be a number";
        return false;
    }
    const float before = goodwill(faction);
    float after = std::clamp(before + delta, rules_.goodwillMin, rules_.goodwillMax);
    if (def->flag("permanentEnemy")) {
        after = rules_.goodwillMin;
    }
    auto it = std::lower_bound(state_.standings.begin(), state_.standings.end(), faction,
                               [](const Standing& s, const std::string& n) { return s.faction < n; });
    if (it == state_.standings.end() || it->faction != faction) {
        it = state_.standings.insert(it, Standing{faction, before});
    }
    it->goodwill = after;
    if (after != before) {
        emit(EventType::GoodwillChanged, 0, kNoEntity, after - before, faction);
        if (delta < 0.0f) {
            sendAwayIfHostile(faction);
        }
    }
    return true;
}

Relation TradeSystem::relation(const std::string& faction) const {
    const Def* def = defs_.find("Factions", faction);
    if (def != nullptr && (def->flag("permanentEnemy") || def->flag("hostile"))) {
        return Relation::Hostile;
    }
    const float g = goodwill(faction);
    return g <= rules_.hostileAtOrBelow ? Relation::Hostile : g >= rules_.alliedAtOrAbove ? Relation::Ally : Relation::Neutral;
}

void TradeSystem::sendAwayIfHostile(const std::string& faction) {
    if (!hostile(faction)) {
        return;
    }
    for (Visit& visit : state_.visits) {
        if (visit.faction == faction && visit.kind == VisitKind::Settlement && visit.phase == VisitPhase::Staying) {
            defend(visit);
        } else if (visit.faction == faction && visit.kind != VisitKind::Orbital && visit.phase == VisitPhase::Staying) {
            leave(visit);
        }
    }
}

std::string TradeSystem::factionOf(Entity pawn) const {
    const FactionId id = fighters_.factionOf(pawn);
    if (id == kPlayerFaction) {
        return std::string();
    }
    for (const DefId def : defs_.idsOfKind("Factions")) {
        if (raids_.factionId(defs_.get(def).defName()) == id) {
            return defs_.get(def).defName();
        }
    }
    return std::string();
}

bool TradeSystem::gift(uint32_t visitId, const std::vector<TradeLine>& items, std::string& error) {
    Visit* visit = nullptr;
    for (Visit& v : state_.visits) {
        if (v.id == visitId) {
            visit = &v;
        }
    }
    if (visit == nullptr) {
        error = "no such visit";
        return false;
    }
    if (visit->phase != VisitPhase::Staying) {
        error = visit->phase == VisitPhase::Defending ? "they are fighting the colony" : "they are leaving";
        return false;
    }
    if (items.empty()) {
        error = "nothing to give";
        return false;
    }
    // A ship takes gifts as it trades: over the comms console, from the beacons' cells.
    std::vector<CellPos> beacon;
    const std::vector<CellPos>* from = nullptr;
    if (visit->kind == VisitKind::Orbital) {
        error = orbitalBlocker(kNoEntity);
        if (!error.empty()) {
            return false;
        }
        beacon = beaconCells();
        from = &beacon;
    }
    std::vector<TradeLine> gifts;
    for (const TradeLine& l : items) {
        if (l.count <= 0 || l.count > 1000000) {
            error = "a gift line needs a count from 1 to 1,000,000";
            return false;
        }
        tradeAddStock(gifts, l.thing, l.count);
    }
    double value = 0.0;
    for (const TradeLine& l : gifts) {
        const Def* def = defs_.find("Things", l.thing);
        if (def == nullptr) {
            error = "no such thing '" + l.thing + "'";
            return false;
        }
        const double each = def->number("stats.marketValue", l.thing == "Silver" ? 1.0 : 0.0);
        if (each <= 0.0) {
            error = def->label() + " is worth nothing as a gift";
            return false;
        }
        if (countOnMap(l.thing, from) < l.count) {
            error = "the colony hasn't " + std::to_string(l.count) + " of " + def->label() + (from != nullptr ? " by a trade beacon" : "");
            return false;
        }
        value += each * l.count;
    }
    for (const TradeLine& l : gifts) {
        takeFromMap(l.thing, l.count, from);
        if (l.thing == "Silver") {
            visit->silver += l.count;
        } else if (!visit->traderKind.empty()) {
            tradeAddStock(visit->stock, l.thing, l.count);
        }
    }
    std::string ignored;
    adjustGoodwill(visit->faction, static_cast<float>(value / rules_.giftSilverPerPoint), ignored);
    return true;
}

void TradeSystem::noteExecuted(Entity prisoner) {
    const std::string faction = factionOf(prisoner);
    std::string ignored;
    if (!faction.empty()) {
        adjustGoodwill(faction, rules_.executedGoodwill, ignored);
    }
}

// ---- Starting a visit -------------------------------------------------------------------------

bool TradeSystem::readKind(const std::string& kindName, KindInfo& out, std::string& error) const {
    const Def* kindDef = defs_.find("PawnKinds", kindName);
    if (kindDef == nullptr) {
        error = "no PawnKinds def '" + kindName + "'";
        return false;
    }
    out.defName = kindDef->defName();
    out.race = kindDef->text("race", "Human");
    if (!defs_.idOf("Things", out.race).valid()) {
        error = "pawn kind '" + kindName + "' has an unknown race";
        return false;
    }
    for (size_t w = 0; w < kindDef->count("weapons"); ++w) {
        out.weapons.push_back(kindDef->textAt("weapons", w));
    }
    for (size_t a = 0; a < kindDef->count("apparel"); ++a) {
        const std::string ab = "apparel." + std::to_string(a);
        out.apparel.push_back(KindInfo::Wear{kindDef->text((ab + ".thing").c_str()), static_cast<float>(kindDef->number((ab + ".chance").c_str(), 1.0))});
    }
    return true;
}

bool TradeSystem::nearestFree(float x, float y, CellPos from, int radius, CellPos& out) const {
    const CellPos c0{static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), 0};
    Regions& regions = map_.regions();
    for (int r = 0; r <= radius; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) {
                    continue;
                }
                const CellPos c{c0.x + dx, c0.y + dy, 0};
                if (!map_.inBounds(c) || !map_.walkable(c) || map_.obstacleDm(c) > 0 || map_.isDoor(c)) {
                    continue;
                }
                if (from.x >= 0 && !regions.canReach(from, c)) {
                    continue;
                }
                out = c;
                return true;
            }
        }
    }
    return false;
}

// A cell near an edge (0 north, 1 east, 2 south, 3 west, -1 any) that can reach `target`.
bool TradeSystem::pickEntry(const CellPos& target, int edge, CellPos& entry) const {
    const int w = map_.width();
    const int h = map_.height();
    const int firstSide = edge >= 0 ? edge : static_cast<int>(rng_.below(4));
    Regions& regions = map_.regions();
    for (int attempt = 0; attempt < (edge >= 0 ? 40 : 160); ++attempt) {
        const int side = (firstSide + attempt / 40) % 4;
        const int along = static_cast<int>(rng_.below(static_cast<uint32_t>(side % 2 == 0 ? w : h)));
        const int d = rules_.edgeDepth;
        const CellPos guess = side == 0 ? CellPos{along, h - 1 - d, 0}
                            : side == 1 ? CellPos{w - 1 - d, along, 0}
                            : side == 2 ? CellPos{along, d, 0}
                                        : CellPos{d, along, 0};
        CellPos c;
        if (nearestFree(tradeCentre(guess.x), tradeCentre(guess.y), CellPos{-1, -1, 0}, 6, c) && regions.canReach(c, target)) {
            entry = c;
            return true;
        }
    }
    return false;
}

bool TradeSystem::spawnPawn(Visit& visit, const KindInfo& kind, VisitorRole role, CellPos cell, uint64_t base, bool defending) {
    entt::registry& reg = things_.registry();
    const Entity pawn = things_.spawn(kind.race, cell);
    if (pawn == kNoEntity || !reg.all_of<Mover, Posture>(pawn)) {
        things_.destroy(pawn);
        return false;
    }
    Colonist person;
    std::string ignored;
    if (!generateColonist(defs_, base, false, person, ignored)) {
        things_.destroy(pawn);
        return false;
    }
    if (looks_) {
        looks_(pawn, base, person.female);
    }
    reg.emplace<Colonist>(pawn, std::move(person));
    reg.emplace<Faction>(pawn, Faction{visit.side});
    if (!defending) {
        reg.emplace<Visitor>(pawn, Visitor{visit.id, role});
    }
    if (!kind.weapons.empty()) {
        const std::string& gun = kind.weapons[rng_.below(static_cast<uint32_t>(kind.weapons.size()))];
        const Entity item = things_.spawn(gun, cell);
        if (item != kNoEntity && !combat_.equip(pawn, item)) {
            things_.destroy(item);
        }
    }
    for (const KindInfo::Wear& wear : kind.apparel) {
        if (rng_.unit() < wear.chance) {
            const Entity item = things_.spawn(wear.thing, cell);
            if (item != kNoEntity && !combat_.equip(pawn, item)) {
                things_.destroy(item);
            }
        }
    }
    // Walking as a fighter in the Flee mode: it never looks for enemies or shoots, it only goes to its goal. A settlement's
    // defender fights, from a post in its base.
    Fighter& f = fighters_.enlist(pawn, defending ? "defending" : "visiting");
    f.mode = defending ? FightMode::Fight : FightMode::Flee;
    f.advances = defending;
    f.hasGoal = true;
    f.goal = visit.spot;
    if (defending) {
        f.anchor = visit.spot;
        CellPos post;
        if (settlementCell(visit, post)) {
            f.goal = post;
        }
    }
    fighters_.replan(pawn);
    visit.members.push_back(pawn);
    if (role == VisitorRole::Trader) {
        visit.trader = pawn;
    }
    return true;
}

bool TradeSystem::startVisit(const std::string& faction, VisitKind kind, const std::string& traderKind, int edge, uint32_t& id,
                             std::string& error) {
    const Def* factionDef = defs_.find("Factions", faction);
    if (factionDef == nullptr) {
        error = "no Factions def '" + faction + "'";
        return false;
    }
    if (edge < -1 || edge > 3) {
        error = "the edge must be -1 (any) or 0 to 3";
        return false;
    }
    if (kind == VisitKind::Settlement) {
        error = "a settlement's people come with its map";
        return false;
    }
    if (kind == VisitKind::Visitors ? !factionDef->flag("canVisit") : !factionDef->flag("canTrade")) {
        error = "the " + factionDef->label() + " do not " + (kind == VisitKind::Visitors ? "send visitors" : "trade");
        return false;
    }
    if (relation(faction) == Relation::Hostile) {
        error = "the " + factionDef->label() + " are hostile to the colony";
        return false;
    }
    entt::registry& reg = things_.registry();

    // The trader kind: asked for, else one of the faction's for that way of coming.
    std::string chosenTrader;
    if (kind != VisitKind::Visitors) {
        const char* list = kind == VisitKind::Caravan ? "traders.caravan" : "traders.orbital";
        std::vector<std::string> options;
        for (size_t i = 0; i < factionDef->count(list); ++i) {
            options.push_back(factionDef->textAt(list, i));
        }
        if (!traderKind.empty()) {
            chosenTrader = traderKind;
        } else if (!options.empty()) {
            chosenTrader = options[rng_.below(static_cast<uint32_t>(options.size()))];
        }
        if (defs_.find("TraderKinds", chosenTrader) == nullptr) {
            error = "the " + factionDef->label() + " have no trader kind for that";
            return false;
        }
    }

    // The colony's middle: its colonists' average, else the map's.
    float sx = 0.0f;
    float sy = 0.0f;
    int n = 0;
    for (const auto [pawn, colonist, pos] : reg.view<const Colonist, const Position>().each()) {
        if (fighters_.factionOf(pawn) == kPlayerFaction && !health_.incapacitated(pawn)) {
            sx += pos.x;
            sy += pos.y;
            ++n;
        }
    }
    const float mx = n > 0 ? sx / static_cast<float>(n) : static_cast<float>(map_.width()) * 0.5f;
    const float my = n > 0 ? sy / static_cast<float>(n) : static_cast<float>(map_.height()) * 0.5f;
    CellPos spot;
    if (!nearestFree(mx, my, CellPos{-1, -1, 0}, 40, spot)) {
        error = "nowhere for visitors to go";
        return false;
    }

    Visit visit;
    visit.kind = kind;
    visit.faction = faction;
    visit.traderKind = chosenTrader;
    visit.side = raids_.factionId(faction);
    visit.arrivedAt = now();
    visit.spot = spot;
    visit.entry = spot;
    const float hour = static_cast<float>(kSecondsPerHour);
    float stayHours = rules_.orbitalStayHours;
    if (kind == VisitKind::Caravan) {
        stayHours = rules_.caravanStayHoursMin + rng_.unit() * (rules_.caravanStayHoursMax - rules_.caravanStayHoursMin);
    } else if (kind == VisitKind::Visitors) {
        stayHours = rules_.visitorStayHoursMin + rng_.unit() * (rules_.visitorStayHoursMax - rules_.visitorStayHoursMin);
    }
    visit.leaveAt = now() + static_cast<int64_t>(stayHours * hour);
    visit.nextWander = now() + rules_.wanderEverySeconds;

    // The trader's silver and stock.
    if (kind != VisitKind::Visitors) {
        rollStock(visit);
    }

    id = 0;
    if (kind == VisitKind::Orbital) {
        visit.id = state_.nextVisit++;
        id = visit.id;
        state_.visits.push_back(std::move(visit));
        const Visit& added = state_.visits.back();
        emit(EventType::VisitArrived, added.id, kNoEntity, static_cast<float>(kind), added.faction);
        return true;
    }

    if (!pickEntry(spot, edge, visit.entry)) {
        error = "no way in from that edge to the colony";
        return false;
    }

    // The pawns to bring: the trader and its guards, or a group of villagers.
    std::vector<std::pair<std::string, VisitorRole>> bring;
    if (kind == VisitKind::Caravan) {
        const int guards = rules_.guardCountMin + static_cast<int>(rng_.below(static_cast<uint32_t>(rules_.guardCountMax - rules_.guardCountMin + 1)));
        if (!pickKinds(*factionDef, "traderKinds", VisitorRole::Trader, 1, bring, error) ||
            !pickKinds(*factionDef, "traderGuardKinds", VisitorRole::Guard, guards, bring, error)) {
            return false;
        }
    } else {
        const int count = rules_.visitorCountMin + static_cast<int>(rng_.below(static_cast<uint32_t>(rules_.visitorCountMax - rules_.visitorCountMin + 1)));
        if (!pickKinds(*factionDef, "visitorKinds", VisitorRole::Visitor, count, bring, error)) {
            return false;
        }
    }
    std::vector<KindInfo> infos;
    for (const auto& m : bring) {
        KindInfo info;
        if (!readKind(m.first, info, error)) {
            return false;
        }
        infos.push_back(std::move(info));
    }

    // Cells for them around the entry, a metre apart (every other cell).
    const std::vector<CellPos> cells = cellsAround(visit.entry, bring.size());
    if (cells.size() < bring.size()) {
        error = "no room at the edge for the visitors";
        return false;
    }

    visit.id = state_.nextVisit;
    spawnGroup(visit, bring, infos, cells, false);
    if (visit.members.empty() || (kind == VisitKind::Caravan && visit.trader == kNoEntity)) {
        for (const Entity m : visit.members) {
            raids_.leaveMap(m);
        }
        error = "the visitors couldn't be made";
        return false;
    }
    ++state_.nextVisit;
    std::sort(visit.members.begin(), visit.members.end());
    visit.started = static_cast<int>(visit.members.size());
    id = visit.id;
    state_.visits.push_back(std::move(visit));
    const Visit& added = state_.visits.back();
    emit(EventType::VisitArrived, added.id, added.trader, static_cast<float>(kind), added.faction);
    return true;
}

bool TradeSystem::pickKinds(const Def& faction, const char* list, VisitorRole role, int count,
                            std::vector<std::pair<std::string, VisitorRole>>& out, std::string& error) const {
    std::vector<std::string> names;
    std::vector<float> weights;
    for (size_t i = 0; i < faction.count(list); ++i) {
        const std::string at = std::string(list) + "." + std::to_string(i);
        if (faction.has((at + ".kind").c_str())) {
            names.push_back(faction.text((at + ".kind").c_str()));
            weights.push_back(static_cast<float>(faction.number((at + ".weight").c_str(), 1.0)));
        } else {
            names.push_back(faction.textAt(list, i));
            weights.push_back(1.0f);
        }
    }
    if (names.empty()) {
        error = std::string("the ") + faction.label() + " have no " + list;
        return false;
    }
    float total = 0.0f;
    for (const float w : weights) {
        total += std::max(0.0f, w);
    }
    for (int k = 0; k < count; ++k) {
        float pick = rng_.unit() * total;
        size_t which = names.size() - 1;
        for (size_t i = 0; i < names.size(); ++i) {
            if (pick < weights[i]) {
                which = i;
                break;
            }
            pick -= weights[i];
        }
        out.emplace_back(names[which], role);
    }
    return true;
}

std::vector<CellPos> TradeSystem::cellsAround(CellPos from, size_t count) const {
    std::vector<CellPos> cells;
    std::vector<CellPos> open{from};
    std::set<int> seen{map_.index(from)};
    for (size_t head = 0; head < open.size() && cells.size() < count && open.size() < 20000; ++head) {
        const CellPos c = open[head];
        if ((c.x + c.y) % 2 == 0 && map_.obstacleDm(c) == 0 && !map_.isDoor(c)) {
            cells.push_back(c);
        }
        const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
        for (const CellPos& next : around) {
            if (map_.walkable(next) && seen.insert(map_.index(next)).second) {
                open.push_back(next);
            }
        }
    }
    return cells;
}

void TradeSystem::spawnGroup(Visit& visit, const std::vector<std::pair<std::string, VisitorRole>>& bring, const std::vector<KindInfo>& infos,
                             const std::vector<CellPos>& cells, bool defending) {
    for (size_t i = 0; i < bring.size() && i < cells.size(); ++i) {
        const uint64_t base = splitMix64((static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next());
        spawnPawn(visit, infos[i], bring[i].second, cells[i], base, defending);
    }
}

void TradeSystem::rollStock(Visit& visit) {
    visit.stock.clear();
    const Def* traderDef = defs_.find("TraderKinds", visit.traderKind);
    if (traderDef == nullptr) {
        visit.silver = 0;
        return;
    }
    const double silverMin = traderDef->number("silver.0", 0.0);
    const double silverMax = std::max(silverMin, traderDef->number("silver.1", silverMin));
    visit.silver = static_cast<int>(std::round(silverMin + rng_.unit() * (silverMax - silverMin)));
    for (size_t i = 0; i < traderDef->count("stock"); ++i) {
        const std::string at = "stock." + std::to_string(i);
        const std::string thing = traderDef->text((at + ".thing").c_str());
        const int lo = static_cast<int>(traderDef->number((at + ".count.0").c_str(), 0.0));
        const int hi = std::max(lo, static_cast<int>(traderDef->number((at + ".count.1").c_str(), lo)));
        const Def* thingDef = defs_.find("Things", thing);
        if (thingDef == nullptr || thingDef->number("stats.marketValue", 0.0) <= 0.0) {
            continue;  // a thing the game doesn't have yet is skipped (the kind's list is written ahead of the items)
        }
        const int count = lo + static_cast<int>(rng_.below(static_cast<uint32_t>(hi - lo + 1)));
        if (count > 0) {
            tradeAddStock(visit.stock, thing, count);
        }
    }
}

bool TradeSystem::startSettlement(const std::string& faction, CellPos spot, int range, int residents, uint32_t& id, std::string& error) {
    const Def* factionDef = defs_.find("Factions", faction);
    if (factionDef == nullptr) {
        error = "no Factions def '" + faction + "'";
        return false;
    }
    if (!map_.inBounds(spot) || !map_.walkable(spot)) {
        error = "the settlement's middle is not open ground";
        return false;
    }
    const bool defending = hostile(faction);
    Visit visit;
    visit.kind = VisitKind::Settlement;
    visit.phase = defending ? VisitPhase::Defending : VisitPhase::Staying;
    visit.faction = faction;
    visit.side = raids_.factionId(faction);
    visit.arrivedAt = now();
    visit.leaveAt = now();
    visit.nextWander = now() + rules_.wanderEverySeconds;
    visit.entry = spot;
    visit.spot = spot;
    visit.range = std::max(1, range);
    visit.restockAt = now() + static_cast<int64_t>(rules_.settlementRestockDays * static_cast<float>(kSecondsPerDay));

    // Who lives there: fighters of a hostile faction; else a trader (if it trades), half guards, half villagers.
    std::vector<std::pair<std::string, VisitorRole>> bring;
    residents = std::max(1, residents);
    if (defending) {
        if (!pickKinds(*factionDef, factionDef->count("pawnKinds") > 0 ? "pawnKinds" : "guardKinds", VisitorRole::Guard, residents, bring, error)) {
            return false;
        }
    } else {
        std::vector<std::string> base;
        for (size_t i = 0; i < factionDef->count("traders.base"); ++i) {
            if (defs_.find("TraderKinds", factionDef->textAt("traders.base", i)) != nullptr) {
                base.push_back(factionDef->textAt("traders.base", i));
            }
        }
        if (factionDef->flag("canTrade") && !base.empty() && factionDef->count("traderKinds") > 0) {
            visit.traderKind = base[rng_.below(static_cast<uint32_t>(base.size()))];
            rollStock(visit);
            if (!pickKinds(*factionDef, "traderKinds", VisitorRole::Trader, 1, bring, error)) {
                return false;
            }
        }
        const int left = residents - static_cast<int>(bring.size());
        const int guards = (left + 1) / 2;
        const char* guardList = factionDef->count("guardKinds") > 0 ? "guardKinds" : "pawnKinds";
        const char* villagerList = factionDef->count("visitorKinds") > 0 ? "visitorKinds" : guardList;
        if (!pickKinds(*factionDef, guardList, VisitorRole::Guard, guards, bring, error) ||
            !pickKinds(*factionDef, villagerList, VisitorRole::Visitor, left - guards, bring, error)) {
            return false;
        }
    }
    std::vector<KindInfo> infos;
    for (const auto& m : bring) {
        KindInfo info;
        if (!readKind(m.first, info, error)) {
            return false;
        }
        infos.push_back(std::move(info));
    }
    const std::vector<CellPos> cells = cellsAround(spot, bring.size());
    if (cells.size() < bring.size()) {
        error = "no room in the settlement for its people";
        return false;
    }
    visit.id = state_.nextVisit;
    spawnGroup(visit, bring, infos, cells, defending);
    if (visit.members.empty() || (!visit.traderKind.empty() && visit.trader == kNoEntity)) {
        for (const Entity m : visit.members) {
            raids_.leaveMap(m);
        }
        error = "the settlement's people couldn't be made";
        return false;
    }
    ++state_.nextVisit;
    std::sort(visit.members.begin(), visit.members.end());
    visit.started = static_cast<int>(visit.members.size());
    id = visit.id;
    state_.visits.push_back(std::move(visit));
    Visit& added = state_.visits.back();
    if (!defending) {
        wander(added);  // they go about the base at once
    }
    emit(EventType::VisitArrived, added.id, added.trader, static_cast<float>(VisitKind::Settlement), added.faction);
    return true;
}

bool TradeSystem::settlementCell(const Visit& visit, CellPos& out) const {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const float x = tradeCentre(visit.spot.x) + (rng_.unit() * 2.0f - 1.0f) * static_cast<float>(visit.range);
        const float y = tradeCentre(visit.spot.y) + (rng_.unit() * 2.0f - 1.0f) * static_cast<float>(visit.range);
        if (nearestFree(x, y, visit.spot, 3, out)) {
            return true;
        }
    }
    return false;
}

void TradeSystem::defend(Visit& visit) {
    entt::registry& reg = things_.registry();
    visit.phase = VisitPhase::Defending;
    for (const Entity m : visit.members) {
        reg.remove<Visitor>(m);
        Fighter* f = reg.try_get<Fighter>(m);
        if (f == nullptr || !things_.spawned(m)) {
            continue;
        }
        f->mode = FightMode::Fight;
        f->advances = true;
        f->anchor = visit.spot;
        f->hasGoal = true;
        CellPos post;
        if (settlementCell(visit, post)) {
            f->goal = post;
        }
        fighters_.replan(m);
    }
}

const Visit* TradeSystem::find(uint32_t id) const {
    for (const Visit& v : state_.visits) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}

// ---- Trade ------------------------------------------------------------------------------------

int TradeSystem::colonyHas(const std::string& thing) const {
    return countOnMap(thing, nullptr);
}

int TradeSystem::countOnMap(const std::string& thing, const std::vector<CellPos>* cells) const {
    const DefId def = defs_.idOf("Things", thing);
    if (!def.valid()) {
        return 0;
    }
    const entt::registry& reg = things_.registry();
    int64_t total = 0;
    for (const auto [e, t, stack, pos] : reg.view<const Thing, const StackCount, const Position>().each()) {
        if (t.category == ThingCategory::Item && t.def == def &&
            (cells == nullptr || std::binary_search(cells->begin(), cells->end(), cellOf(pos), tradeCellLess))) {
            total += stack.count;
        }
    }
    return static_cast<int>(std::min<int64_t>(total, 2000000000));
}

// ---- Comms console and trade beacons ------------------------------------------------------------

bool TradeSystem::builtAndRunning(Entity building) const {
    const entt::registry& reg = things_.registry();
    return things_.spawned(building) && !reg.all_of<ConstructionSite>(building) && (!running_ || running_(building));
}

Entity TradeSystem::commsConsole(Entity user) const {
    const entt::registry& reg = things_.registry();
    std::vector<Entity> consoles;
    for (const auto [e, t, fp] : reg.view<const Thing, const Footprint>().each()) {
        if (t.category == ThingCategory::Building && defs_.get(t.def).flag("building.commsConsole") && builtAndRunning(e)) {
            consoles.push_back(e);
        }
    }
    std::sort(consoles.begin(), consoles.end());
    if (user == kNoEntity) {
        return consoles.empty() ? kNoEntity : consoles.front();
    }
    if (!things_.spawned(user)) {
        return kNoEntity;
    }
    const CellPos from = cellOf(reg.get<Position>(user));
    Regions& regions = map_.regions();
    for (const Entity console : consoles) {
        // Any cell beside the console that the user can walk to.
        const CellRect rect = reg.get<Footprint>(console).rect();
        for (int y = rect.minY - 1; y <= rect.minY + rect.sizeY; ++y) {
            for (int x = rect.minX - 1; x <= rect.minX + rect.sizeX; ++x) {
                const CellPos c{x, y, rect.level};
                const bool inside = x >= rect.minX && x < rect.minX + rect.sizeX && y >= rect.minY && y < rect.minY + rect.sizeY;
                if (!inside && map_.walkable(c) && (c == from || regions.canReach(from, c))) {
                    return console;
                }
            }
        }
    }
    return kNoEntity;
}

std::vector<Entity> TradeSystem::tradeBeacons() const {
    const entt::registry& reg = things_.registry();
    std::vector<Entity> beacons;
    for (const auto [e, t, fp] : reg.view<const Thing, const Footprint>().each()) {
        if (t.category == ThingCategory::Building && defs_.get(t.def).flag("building.tradeBeacon") && builtAndRunning(e)) {
            beacons.push_back(e);
        }
    }
    std::sort(beacons.begin(), beacons.end());
    return beacons;
}

std::vector<CellPos> TradeSystem::beaconCells() const {
    const entt::registry& reg = things_.registry();
    std::vector<CellPos> cells;
    for (const Entity beacon : tradeBeacons()) {
        const CellPos at = reg.get<Footprint>(beacon).origin;
        const float radius = static_cast<float>(defs_.get(reg.get<Thing>(beacon).def).number("building.tradeRadius", rules_.beaconRadius));
        const int reach = static_cast<int>(std::ceil(radius));
        const int side = 2 * reach + 1;
        // Spread from the beacon over cells in the radius; a wall, a door or rock stops it (RimWorld: the beacon's district).
        std::vector<uint8_t> seen(static_cast<size_t>(side) * static_cast<size_t>(side), 0);
        std::vector<CellPos> open{at};
        seen[static_cast<size_t>(reach * side + reach)] = 1;
        while (!open.empty()) {
            const CellPos c = open.back();
            open.pop_back();
            cells.push_back(c);
            const int steps[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (const auto& d : steps) {
                const CellPos n{c.x + d[0], c.y + d[1], c.level};
                const int dx = n.x - at.x;
                const int dy = n.y - at.y;
                if (std::abs(dx) > reach || std::abs(dy) > reach ||
                    static_cast<float>(dx * dx + dy * dy) > radius * radius || !map_.inBounds(n)) {
                    continue;
                }
                uint8_t& mark = seen[static_cast<size_t>((dy + reach) * side + dx + reach)];
                if (mark != 0) {
                    continue;
                }
                mark = 1;
                const Passage passage = map_.passage(n);
                if (passage == Passage::Blocked || passage == Passage::Door || map_.terrain(n) == Terrain::Rock) {
                    continue;
                }
                open.push_back(n);
            }
        }
    }
    std::sort(cells.begin(), cells.end(), tradeCellLess);
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

int TradeSystem::beaconHas(const std::string& thing) const {
    const std::vector<CellPos> cells = beaconCells();
    return countOnMap(thing, &cells);
}

std::string TradeSystem::orbitalBlocker(Entity negotiator) const {
    if (commsConsole(kNoEntity) == kNoEntity) {
        return "the colony needs a powered comms console to talk to the ship";
    }
    if (negotiator != kNoEntity && commsConsole(negotiator) == kNoEntity) {
        return "the negotiator can't get to a powered comms console";
    }
    if (tradeBeacons().empty()) {
        return "the colony needs a powered orbital trade beacon to trade with a ship";
    }
    return std::string();
}

CellPos TradeSystem::dropSpot(const Visit& visit) const {
    if (visit.kind == VisitKind::Orbital) {
        const std::vector<Entity> beacons = tradeBeacons();
        if (!beacons.empty()) {
            return things_.registry().get<Footprint>(beacons.front()).origin;
        }
    }
    return visit.spot;
}

bool TradeSystem::requestCaravan(const std::string& faction, const std::string& traderKind, Entity negotiator, std::string& error) {
    if (owner_ != nullptr) {
        error = "only the colony's own map has a comms console to call from";
        return false;
    }
    const Def* factionDef = defs_.find("Factions", faction);
    if (factionDef == nullptr) {
        error = "no faction '" + faction + "'";
        return false;
    }
    if (!factionDef->flag("canTrade")) {
        error = "the " + factionDef->label() + " don't send traders";
        return false;
    }
    if (relation(faction) != Relation::Ally) {
        error = "only an allied faction sends a caravan when asked";
        return false;
    }
    if (!traderKind.empty()) {
        bool listed = false;
        for (size_t i = 0; i < factionDef->count("traders.caravan"); ++i) {
            listed = listed || factionDef->textAt("traders.caravan", i) == traderKind;
        }
        if (!listed) {
            error = "the " + factionDef->label() + " have no '" + traderKind + "' caravans";
            return false;
        }
    }
    const entt::registry& reg = things_.registry();
    if (!things_.spawned(negotiator) || !reg.all_of<Mover>(negotiator) || fighters_.factionOf(negotiator) != kPlayerFaction ||
        health_.incapacitated(negotiator)) {
        error = "the caller must be one of the colony's pawns who can act";
        return false;
    }
    if (commsConsole(kNoEntity) == kNoEntity) {
        error = "the colony needs a powered comms console";
        return false;
    }
    if (commsConsole(negotiator) == kNoEntity) {
        error = "the caller can't get to a powered comms console";
        return false;
    }
    const int64_t t = now();
    const int64_t cooldown = static_cast<int64_t>(rules_.requestCooldownDays * static_cast<float>(kSecondsPerDay));
    for (const CommsCall& call : state_.calls) {
        if (call.faction == faction && t < call.at + cooldown) {
            const int64_t hours = (call.at + cooldown - t + kSecondsPerHour - 1) / kSecondsPerHour;
            error = "the " + factionDef->label() + " were asked lately; ask again in " + std::to_string(hours) + " hours";
            return false;
        }
    }
    if (!adjustGoodwill(faction, -rules_.requestGoodwillCost, error)) {
        return false;
    }
    auto call = std::lower_bound(state_.calls.begin(), state_.calls.end(), faction,
                                 [](const CommsCall& c, const std::string& f) { return c.faction < f; });
    if (call != state_.calls.end() && call->faction == faction) {
        call->at = t;
    } else {
        state_.calls.insert(call, CommsCall{faction, t});
    }
    CaravanRequest request;
    request.faction = faction;
    request.traderKind = traderKind;
    request.arriveAt = t + static_cast<int64_t>(rules_.requestArrivalDays * static_cast<float>(kSecondsPerDay));
    const auto at = std::upper_bound(state_.requests.begin(), state_.requests.end(), request, [](const CaravanRequest& a, const CaravanRequest& b) {
        return a.arriveAt != b.arriveAt ? a.arriveAt < b.arriveAt : a.faction < b.faction;
    });
    state_.requests.insert(at, request);
    emit(EventType::CaravanRequested, 0, negotiator, rules_.requestArrivalDays, faction);
    return true;
}

// Asked-for caravans whose day has come walk in. One that can't (the faction turned hostile, no way in) is dropped.
void TradeSystem::arriveRequested() {
    const int64_t t = now();
    while (!state_.requests.empty() && state_.requests.front().arriveAt <= t) {
        const CaravanRequest request = state_.requests.front();
        state_.requests.erase(state_.requests.begin());
        uint32_t id = 0;
        std::string error;
        if (!startVisit(request.faction, VisitKind::Caravan, request.traderKind, -1, id, error)) {
            emit(EventType::CaravanRequestFailed, 0, kNoEntity, 0.0f, request.faction + ": " + error);
        }
    }
    // Calls whose cooldown is over are forgotten, so a quiet game keeps no state.
    const int64_t cooldown = static_cast<int64_t>(rules_.requestCooldownDays * static_cast<float>(kSecondsPerDay));
    state_.calls.erase(std::remove_if(state_.calls.begin(), state_.calls.end(), [&](const CommsCall& c) { return t >= c.at + cooldown; }),
                       state_.calls.end());
}

int TradeSystem::silverOnMap() const {
    return colonyHas("Silver");
}

float TradeSystem::priceImprovement(Entity negotiator) const {
    float improvement = 0.0f;
    if (things_.registry().valid(negotiator)) {
        improvement = tradeCurve(rules_.improvementBySocial, static_cast<float>(jobs_.skillLevel(negotiator, "Social")));
        if (talents_ != nullptr) {
            improvement += 1.0f - static_cast<float>(talents_->statFactor(negotiator, "tradePrice"));
        }
    }
    improvement -= disadvantage_;
    return std::clamp(improvement, rules_.improvementFloor, rules_.improvementCap);
}

std::vector<std::string> TradeSystem::wordsOf(const Def& def) const {
    std::vector<std::string> words;
    for (size_t i = 0; i < def.count("thingCategories"); ++i) {
        words.push_back(def.textAt("thingCategories", i));
    }
    if (tradeHas(words, "FoodRaw") || tradeHas(words, "PreparedFood")) {
        words.push_back("Foods");
    }
    if (tradeHas(words, "Leathers")) {
        words.push_back("Textiles");
    }
    if (tradeHas(words, "BodyPartsArtificial")) {
        words.push_back("Bionics");
    }
    if (def.has("apparel")) {
        words.push_back("Apparel");
    }
    if (def.has("equipmentType") || def.has("tools") || def.has("verbs")) {
        words.push_back("Weapons");
    }
    if (def.has("stuffProps")) {
        words.push_back("Stuff");
        for (size_t i = 0; i < def.count("stuffProps.categories"); ++i) {
            const std::string category = def.textAt("stuffProps.categories", i);
            if (category == "Metallic") {
                words.push_back("Metals");
            } else if (category == "Fabric" || category == "Leathery") {
                words.push_back("Textiles");
            }
        }
    }
    if (def.defName() == "ComponentSpacer") {
        words.push_back("AdvancedComponents");
    }
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    return words;
}

float TradeSystem::sellFactor(const Def& def) const {
    float factor = 1.0f;
    for (const std::string& word : wordsOf(def)) {
        const auto it = rules_.sellCategoryFactors.find(word);
        if (it != rules_.sellCategoryFactors.end()) {
            factor *= it->second;
        }
    }
    return factor;
}

bool TradeSystem::accepts(const Visit& visit, const Def& def) const {
    if (visit.traderKind.empty() || def.number("stats.marketValue", 0.0) <= 0.0 || def.defName() == "Silver") {
        return false;
    }
    if (tradeHas(rules_.alwaysAccepted, def.defName())) {
        return true;
    }
    const Def* kind = defs_.find("TraderKinds", visit.traderKind);
    if (kind == nullptr) {
        return false;
    }
    const std::vector<std::string> words = wordsOf(def);
    if (kind->flag("refusesRawPlants") && tradeHas(words, "PlantFoodRaw")) {
        return false;
    }
    for (size_t i = 0; i < kind->count("buys"); ++i) {
        if (tradeHas(words, kind->textAt("buys", i))) {
            return true;
        }
    }
    return false;
}

float TradeSystem::unitPrice(const Def& def, bool colonyBuys, float improvement) const {
    const float value = static_cast<float>(def.number("stats.marketValue", 0.0));
    if (colonyBuys) {
        return value * rules_.colonyBuysAt * (1.0f - improvement);
    }
    return value * rules_.colonySellsAt * sellFactor(def) * (1.0f + improvement);
}

int TradeSystem::takeFromMap(const std::string& thing, int count, const std::vector<CellPos>* cells) {
    const DefId def = defs_.idOf("Things", thing);
    entt::registry& reg = things_.registry();
    std::vector<Entity> stacks;
    for (const auto [e, t, stack, pos] : reg.view<const Thing, const StackCount, const Position>().each()) {
        if (t.category == ThingCategory::Item && t.def == def &&
            (cells == nullptr || std::binary_search(cells->begin(), cells->end(), cellOf(pos), tradeCellLess))) {
            stacks.push_back(e);
        }
    }
    std::sort(stacks.begin(), stacks.end());
    int taken = 0;
    for (const Entity e : stacks) {
        if (taken >= count) {
            break;
        }
        StackCount& stack = reg.get<StackCount>(e);
        const int n = std::min(stack.count, count - taken);
        taken += n;
        if (n >= stack.count) {
            things_.destroy(e);
        } else {
            stack.count -= n;
        }
    }
    return taken;
}

// Puts `count` of a thing down near `at`, in stacks.
void TradeSystem::dropGoods(CellPos at, const std::string& thing, int count) {
    const Def* def = defs_.find("Things", thing);
    if (def == nullptr) {
        return;
    }
    const int limit = std::max(1, static_cast<int>(def->integer("stackLimit", 1)));
    Regions& regions = map_.regions();
    while (count > 0) {
        const int n = std::min(count, limit);
        CellPos cell = at;
        bool found = false;
        for (int r = 0; r <= rules_.dropRadius && !found; ++r) {
            for (int dy = -r; dy <= r && !found; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) {
                        continue;
                    }
                    const CellPos c{at.x + dx, at.y + dy, at.level};
                    if (map_.inBounds(c) && map_.walkable(c) && map_.obstacleDm(c) == 0 && !map_.isDoor(c) &&
                        things_.itemsAt(c).empty() && regions.canReach(at, c)) {
                        cell = c;
                        found = true;
                        break;
                    }
                }
            }
        }
        if (things_.spawn(thing, cell, n) == kNoEntity) {
            break;
        }
        count -= n;
    }
}

bool TradeSystem::trade(uint32_t visitId, Entity negotiator, const std::vector<TradeLine>& buy, const std::vector<TradeLine>& sell,
                        std::string& error) {
    const entt::registry& reg = things_.registry();
    Visit* visit = nullptr;
    for (Visit& v : state_.visits) {
        if (v.id == visitId) {
            visit = &v;
        }
    }
    if (visit == nullptr) {
        error = "no such visit";
        return false;
    }
    if (visit->traderKind.empty()) {
        error = "those visitors have nothing to trade";
        return false;
    }
    if (visit->phase != VisitPhase::Staying) {
        error = visit->phase == VisitPhase::Defending ? "the " + visit->faction + " are fighting the colony" : "the trader is leaving";
        return false;
    }
    if (visit->kind != VisitKind::Orbital &&
        (visit->trader == kNoEntity || !things_.spawned(visit->trader) || health_.incapacitated(visit->trader))) {
        error = "the trader can't trade now";
        return false;
    }
    if (!things_.spawned(negotiator) || !reg.all_of<Mover>(negotiator) || fighters_.factionOf(negotiator) != kPlayerFaction ||
        health_.incapacitated(negotiator)) {
        error = "the negotiator must be one of the colony's pawns who can act";
        return false;
    }
    if (buy.empty() && sell.empty()) {
        error = "nothing to trade";
        return false;
    }
    // A ship is reached over a powered comms console, and what the colony sells or pays lies by a powered trade beacon.
    std::vector<CellPos> beacon;
    const std::vector<CellPos>* from = nullptr;
    if (visit->kind == VisitKind::Orbital) {
        error = orbitalBlocker(negotiator);
        if (!error.empty()) {
            return false;
        }
        beacon = beaconCells();
        from = &beacon;
    }
    // A settlement gives a little better prices (RimWorld's settlement price improvement offset).
    const float improvement = visit->kind == VisitKind::Settlement
                                  ? std::min(rules_.improvementCap, priceImprovement(negotiator) + rules_.settlementDiscount)
                                  : priceImprovement(negotiator);

    // Merge the lines by thing and check each.
    std::vector<TradeLine> buys;
    std::vector<TradeLine> sells;
    for (const TradeLine& l : buy) {
        if (l.count <= 0 || l.count > 1000000) {
            error = "a trade line needs a count from 1 to 1,000,000";
            return false;
        }
        tradeAddStock(buys, l.thing, l.count);
    }
    for (const TradeLine& l : sell) {
        if (l.count <= 0 || l.count > 1000000) {
            error = "a trade line needs a count from 1 to 1,000,000";
            return false;
        }
        tradeAddStock(sells, l.thing, l.count);
    }
    double buyTotal = 0.0;
    double sellTotal = 0.0;
    for (const TradeLine& l : buys) {
        const Def* def = defs_.find("Things", l.thing);
        if (def == nullptr || def->number("stats.marketValue", 0.0) <= 0.0 || l.thing == "Silver") {
            error = "'" + l.thing + "' can't be bought";
            return false;
        }
        const auto have = std::lower_bound(visit->stock.begin(), visit->stock.end(), l.thing,
                                           [](const TradeLine& s, const std::string& n) { return s.thing < n; });
        if (have == visit->stock.end() || have->thing != l.thing || have->count < l.count) {
            error = "the trader hasn't " + std::to_string(l.count) + " of " + def->label();
            return false;
        }
        buyTotal += static_cast<double>(unitPrice(*def, true, improvement)) * l.count;
    }
    for (const TradeLine& l : sells) {
        const Def* def = defs_.find("Things", l.thing);
        if (def == nullptr || def->kind() != "Things" || !accepts(*visit, *def)) {
            error = "the trader won't take '" + l.thing + "'";
            return false;
        }
        if (countOnMap(l.thing, from) < l.count) {
            error = "the colony hasn't " + std::to_string(l.count) + " of " + def->label() + " to sell" +
                    (from != nullptr ? " by a trade beacon" : "");
            return false;
        }
        sellTotal += static_cast<double>(unitPrice(*def, false, improvement)) * l.count;
    }
    // Whole silver: the colony pays rounded up and is paid rounded down.
    const int cost = static_cast<int>(std::ceil(buyTotal - 1e-4));
    const int gain = static_cast<int>(std::floor(sellTotal + 1e-4));
    const int net = gain - cost;  // silver the trader owes the colony, or minus what the colony owes it
    int paid = 0;
    int surplus = 0;
    if (net < 0) {
        const int silver = countOnMap("Silver", from);
        if (silver < -net) {
            error = "the deal costs " + std::to_string(-net) + " silver and the colony has " + std::to_string(silver) +
                    (from != nullptr ? " by a trade beacon" : "");
            return false;
        }
    } else {
        paid = std::min(net, visit->silver);
        surplus = net - paid;
    }

    // Do it.
    const CellPos dropAt = dropSpot(*visit);
    for (const TradeLine& l : sells) {
        takeFromMap(l.thing, l.count, from);
        tradeAddStock(visit->stock, l.thing, l.count);
    }
    for (const TradeLine& l : buys) {
        for (TradeLine& s : visit->stock) {
            if (s.thing == l.thing) {
                s.count -= l.count;
            }
        }
        dropGoods(dropAt, l.thing, l.count);
    }
    visit->stock.erase(std::remove_if(visit->stock.begin(), visit->stock.end(), [](const TradeLine& s) { return s.count <= 0; }),
                       visit->stock.end());
    if (net < 0) {
        takeFromMap("Silver", -net, from);
        visit->silver += -net;
    } else if (paid > 0) {
        visit->silver -= paid;
        dropGoods(dropAt, "Silver", paid);
    }
    emit(EventType::TradeDone, visit->id, visit->trader, static_cast<float>(net),
         surplus > 0 ? visit->faction + ": " + std::to_string(surplus) + " silver of goods given free" : visit->faction);
    return true;
}

// ---- Each step --------------------------------------------------------------------------------

CellPos TradeSystem::edgeGoal(Entity pawn) const {
    const entt::registry& reg = things_.registry();
    const CellPos here = cellOf(reg.get<Position>(pawn));
    const int w = map_.width();
    const int h = map_.height();
    struct Side {
        int d;
        int edge;
    };
    Side sides[4] = {{h - 1 - here.y, 0}, {w - 1 - here.x, 1}, {here.y, 2}, {here.x, 3}};
    std::sort(std::begin(sides), std::end(sides), [](const Side& a, const Side& b) { return a.d != b.d ? a.d < b.d : a.edge < b.edge; });
    Regions& regions = map_.regions();
    for (const Side& s : sides) {
        for (int off = 0; off <= 40; ++off) {
            for (const int sign : {1, -1}) {
                const int along = (s.edge % 2 == 0 ? here.x : here.y) + sign * off;
                const CellPos c = s.edge == 0 ? CellPos{along, h - 1, 0}
                                : s.edge == 1 ? CellPos{w - 1, along, 0}
                                : s.edge == 2 ? CellPos{along, 0, 0}
                                              : CellPos{0, along, 0};
                if (map_.inBounds(c) && map_.walkable(c) && regions.canReach(here, c)) {
                    return c;
                }
            }
        }
    }
    return here;
}

void TradeSystem::leave(Visit& visit) {
    entt::registry& reg = things_.registry();
    visit.phase = VisitPhase::Leaving;
    visit.leaveAt = now();
    for (const Entity m : visit.members) {
        Fighter* f = reg.try_get<Fighter>(m);
        if (f == nullptr || !things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        f->mode = FightMode::Flee;
        f->hasGoal = true;
        f->goal = edgeGoal(m);
        fighters_.replan(m);
    }
}

void TradeSystem::wander(Visit& visit) {
    entt::registry& reg = things_.registry();
    visit.nextWander = now() + rules_.wanderEverySeconds;
    for (const Entity m : visit.members) {
        const Visitor* v = reg.try_get<Visitor>(m);
        Fighter* f = reg.try_get<Fighter>(m);
        // A settlement's guards walk about too; a trader always stands at the spot.
        const bool walks = v != nullptr && (v->role == VisitorRole::Visitor || (visit.kind == VisitKind::Settlement && v->role == VisitorRole::Guard));
        if (!walks || f == nullptr || !things_.spawned(m) || health_.incapacitated(m)) {
            continue;
        }
        CellPos goal;
        bool found = false;
        if (visit.kind == VisitKind::Settlement) {
            found = settlementCell(visit, goal);
        } else {
            const float angle = rng_.unit() * 6.2831853f;
            const float dist = rng_.unit() * rules_.wanderRadius;
            found = nearestFree(tradeCentre(visit.spot.x) + std::cos(angle) * dist, tradeCentre(visit.spot.y) + std::sin(angle) * dist,
                                visit.spot, 3, goal);
        }
        if (found) {
            f->hasGoal = true;
            f->goal = goal;
            fighters_.replan(m);
        }
    }
}

void TradeSystem::step(const std::vector<Event>& events) {
    const int64_t t = now();
    const entt::registry& eventReg = things_.registry();
    for (size_t i = 0; i < events.size(); ++i) {
        const Event& e = events[i];
        if (e.type == EventType::PrisonerReleased && eventReg.valid(e.thing)) {
            const std::string faction = factionOf(e.thing);
            std::string ignored;
            if (!faction.empty()) {
                adjustGoodwill(faction, rules_.releaseGoodwill, ignored);
            }
        } else if (e.type == EventType::PawnDied && eventReg.valid(e.thing) && eventReg.all_of<Visitor>(e.thing)) {
            std::string ignored;
            adjustGoodwill(factionOf(e.thing), rules_.visitorKilledGoodwill, ignored);
        }
    }
    if (!state_.standings.empty() && t / kSecondsPerDay > state_.lastDrift) {
        // Goodwill drifts back toward where the faction started, a little every day.
        const float days = static_cast<float>(t / kSecondsPerDay - state_.lastDrift);
        state_.lastDrift = t / kSecondsPerDay;
        for (auto it = state_.standings.begin(); it != state_.standings.end();) {
            const Def* def = defs_.find("Factions", it->faction);
            const float home = startGoodwill(it->faction);
            if (def != nullptr && !def->flag("permanentEnemy")) {
                const float pull = rules_.driftPerDay * days;
                it->goodwill = it->goodwill < home ? std::min(home, it->goodwill + pull) : std::max(home, it->goodwill - pull);
            }
            if (it->goodwill == home) {
                it = state_.standings.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!state_.requests.empty() || !state_.calls.empty()) {
        arriveRequested();
    }
    if (state_.visits.empty() || t % 15 != 0) {
        return;
    }
    entt::registry& reg = things_.registry();
    const int w = map_.width();
    const int h = map_.height();
    for (size_t i = 0; i < state_.visits.size();) {
        Visit& visit = state_.visits[i];
        visit.members.erase(std::remove_if(visit.members.begin(), visit.members.end(),
                                           [&](Entity m) {
                                               const Health* hp = reg.valid(m) ? reg.try_get<Health>(m) : nullptr;
                                               return !reg.valid(m) || !things_.spawned(m) || (hp != nullptr && hp->dead);
                                           }),
                            visit.members.end());
        if (visit.trader != kNoEntity && std::find(visit.members.begin(), visit.members.end(), visit.trader) == visit.members.end()) {
            visit.trader = kNoEntity;
        }
        bool over = false;
        if (visit.kind == VisitKind::Orbital) {
            over = t >= visit.leaveAt;
        } else if (visit.kind == VisitKind::Settlement) {
            // Its people live there: they go about the base, the trader's goods come again, and they fight once the colony
            // is their enemy. The visit ends only when they are all gone.
            over = visit.members.empty();
            if (!over && visit.phase == VisitPhase::Staying) {
                if (hostile(visit.faction)) {
                    defend(visit);
                } else {
                    if (t >= visit.nextWander) {
                        wander(visit);
                    }
                    if (t >= visit.restockAt) {
                        visit.restockAt = t + static_cast<int64_t>(rules_.settlementRestockDays * static_cast<float>(kSecondsPerDay));
                        if (!visit.traderKind.empty()) {
                            rollStock(visit);
                        }
                    }
                }
            }
        } else if (visit.phase == VisitPhase::Staying) {
            if (visit.members.empty()) {
                over = true;
            } else if (t >= visit.leaveAt) {
                leave(visit);
            } else if (visit.kind == VisitKind::Visitors && t >= visit.nextWander) {
                wander(visit);
            }
        } else {
            std::vector<Entity> gone;
            const bool timedOut = t >= visit.leaveAt + rules_.leaveTimeoutSeconds;
            for (const Entity m : visit.members) {
                const CellPos c = cellOf(reg.get<Position>(m));
                // Within a few cells of the edge counts: a group's goal is shared, so the last ones stop a ring or two short of it.
                if (timedOut || c.x <= kEdgeMargin || c.y <= kEdgeMargin || c.x >= w - 1 - kEdgeMargin || c.y >= h - 1 - kEdgeMargin) {
                    gone.push_back(m);
                }
            }
            for (const Entity m : gone) {
                visit.members.erase(std::find(visit.members.begin(), visit.members.end(), m));
                raids_.leaveMap(m);
            }
            over = visit.members.empty();
        }
        if (over) {
            emit(EventType::VisitLeft, visit.id, kNoEntity, static_cast<float>(visit.kind), visit.faction);
            state_.visits.erase(state_.visits.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void TradeSystem::forget(Entity thing) {
    for (Visit& visit : state_.visits) {
        visit.members.erase(std::remove(visit.members.begin(), visit.members.end(), thing), visit.members.end());
        if (visit.trader == thing) {
            visit.trader = kNoEntity;
        }
    }
}

bool TradeSystem::restore(TradeState state, std::string& error) {
    uint32_t last = 0;
    for (const Visit& v : state.visits) {
        if (v.id == 0 || v.id >= state.nextVisit || v.id <= last || defs_.find("Factions", v.faction) == nullptr ||
            (!v.traderKind.empty() && defs_.find("TraderKinds", v.traderKind) == nullptr)) {
            error = "the save has a visit that doesn't fit the data files";
            return false;
        }
        last = v.id;
        for (const TradeLine& s : v.stock) {
            if (s.count <= 0 || defs_.find("Things", s.thing) == nullptr) {
                error = "the save has trader stock that the data files can't place";
                return false;
            }
        }
    }
    for (const CaravanRequest& r : state.requests) {
        if (defs_.find("Factions", r.faction) == nullptr || (!r.traderKind.empty() && defs_.find("TraderKinds", r.traderKind) == nullptr)) {
            error = "the save has a caravan request that doesn't fit the data files";
            return false;
        }
    }
    std::vector<Standing> kept;
    for (const Standing& s : state.standings) {
        if (defs_.find("Factions", s.faction) != nullptr) {
            kept.push_back(s);  // a faction the data files dropped is forgotten
        }
    }
    state.standings = std::move(kept);
    state_ = std::move(state);
    return true;
}

void TradeSystem::hashState(StateHasher& h) const {
    if (state_.visits.empty() && state_.standings.empty() && state_.nextVisit == 1 && state_.lastDrift == 0 && state_.requests.empty() &&
        state_.calls.empty()) {
        return;  // a game that never met the system hashes as it did before it existed
    }
    h.add(state_.nextVisit);
    h.add(state_.lastDrift);
    h.add(static_cast<uint64_t>(state_.standings.size()));
    for (const Standing& s : state_.standings) {
        h.add(s.faction.data(), s.faction.size());
        h.add(s.goodwill);
    }
    h.add(static_cast<uint64_t>(state_.visits.size()));
    for (const Visit& v : state_.visits) {
        h.add(v.id);
        h.add(static_cast<int>(v.kind));
        h.add(static_cast<int>(v.phase));
        h.add(v.faction.data(), v.faction.size());
        h.add(v.traderKind.data(), v.traderKind.size());
        h.add(v.side);
        h.add(v.arrivedAt);
        h.add(v.leaveAt);
        h.add(v.nextWander);
        h.add(v.entry.x);
        h.add(v.entry.y);
        h.add(v.spot.x);
        h.add(v.spot.y);
        h.add(v.silver);
        h.add(v.started);
        h.add(entt::to_integral(v.trader));
        h.add(static_cast<uint64_t>(v.stock.size()));
        for (const TradeLine& s : v.stock) {
            h.add(s.thing.data(), s.thing.size());
            h.add(s.count);
        }
        h.add(static_cast<uint64_t>(v.members.size()));
        for (const Entity m : v.members) {
            h.add(entt::to_integral(m));
        }
        if (v.kind == VisitKind::Settlement) {
            h.add(v.restockAt);
            h.add(v.range);
        }
    }
    if (!state_.requests.empty() || !state_.calls.empty()) {  // comms: only once a caravan was asked for
        h.add(static_cast<uint64_t>(state_.requests.size()));
        for (const CaravanRequest& r : state_.requests) {
            h.add(r.faction.data(), r.faction.size());
            h.add(r.traderKind.data(), r.traderKind.size());
            h.add(r.arriveAt);
        }
        h.add(static_cast<uint64_t>(state_.calls.size()));
        for (const CommsCall& c : state_.calls) {
            h.add(c.faction.data(), c.faction.size());
            h.add(c.at);
        }
    }
}

void TradeSystem::hashThing(Entity e, StateHasher& h) const {
    if (const Visitor* v = things_.registry().try_get<Visitor>(e)) {
        h.add(v->visit);
        h.add(static_cast<int>(v->role));
    }
}

}  // namespace colony

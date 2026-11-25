#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Fighters.h"
#include "ColonySim/MentalBreaks.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace colony {

namespace {

// Walkable cells reachable from `from`, nearest first (4-connected), at most `limit` of them.
std::vector<CellPos> scenarioNearbyCells(const Map& map, CellPos from, size_t limit) {
    std::vector<CellPos> cells{from};
    std::set<int> seen{map.index(from)};
    for (size_t head = 0; head < cells.size() && cells.size() < limit; ++head) {
        const CellPos c = cells[head];
        const CellPos around[4] = {{c.x + 1, c.y, 0}, {c.x - 1, c.y, 0}, {c.x, c.y + 1, 0}, {c.x, c.y - 1, 0}};
        for (const CellPos& n : around) {
            if (map.walkable(n) && seen.insert(map.index(n)).second) {
                cells.push_back(n);
            }
        }
    }
    return cells;
}

// The lineage of a colonist who rolls one: a draw from the scenario's `lineageWeights` with its own
// stream from the colonist's seed (so nothing else in the sim moves), or the scenario's `lineage`
// when it has no weights. Whole-number weights, keys in name order: the same draw everywhere.
std::string scenarioRollLineage(const Def& scenario, uint64_t colonistSeed, const std::string& fallback) {
    std::vector<std::string> names = scenario.keys("lineageWeights");
    std::sort(names.begin(), names.end());
    std::vector<uint32_t> weights;
    uint32_t total = 0;
    for (const std::string& name : names) {
        const double w = std::max(0.0, scenario.number(("lineageWeights." + name).c_str(), 0.0));
        weights.push_back(static_cast<uint32_t>(w * 100.0 + 0.5));
        total += weights.back();
    }
    if (total == 0) {
        return fallback;
    }
    Rng rng(splitMix64(colonistSeed ^ 0x6C696E6561676573ULL), 11);
    uint32_t pick = rng.below(total);
    for (size_t i = 0; i < names.size(); ++i) {
        if (pick < weights[i]) {
            return names[i];
        }
        pick -= weights[i];
    }
    return fallback;
}

// A plant cleared from the landing: trees and anything that slows walking (tall grass, bushes, brambles, cacti).
// Short grass, dandelions and moss stay, so the clearing still looks natural.
bool scenarioClearsPlant(const DefDatabase& defs, const Plants& plants, CellPos c) {
    if (!plants.has(c)) {
        return false;
    }
    const Def& def = defs.get(plants.defAt(c));
    return def.flag("plant.isTree") || def.number("pathCost", 0.0) > 0.0;
}

// Walkable dry ground (no water, marsh, mud or rock) in every cell within `radius` of `c`, as the stockpile's landing.
bool scenarioDryAround(const Map& map, CellPos c, int radius) {
    for (int y = c.y - radius; y <= c.y + radius; ++y) {
        for (int x = c.x - radius; x <= c.x + radius; ++x) {
            const CellPos n{x, y, 0};
            if (!map.walkable(n)) {
                return false;
            }
            const Terrain t = map.terrain(n);
            if (t == Terrain::ShallowWater || t == Terrain::Marsh || t == Terrain::Mud) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// Makes one colonist of `scenario` standing at `cell`, from `colonistSeed`: character, worker,
// looks, and the gear of the scenario's pawn kind (else its `colonistApparel`). Returns kNoEntity
// and a setup error if it can't.
Entity Sim::makeScenarioColonist(const Def& scenario, CellPos cell, uint64_t colonistSeed, bool protagonist) {
    const std::string who = "scenario '" + settings_.scenario + "': ";
    const std::string kindName = scenario.text("colonistKind", "Human");
    const DefId kind = defs_->idOf("Things", kindName);
    const std::string lineage = scenario.text("lineage", "Baseline");
    const Def* gearKind = scenario.text("colonistPawnKind").empty() ? nullptr : defs_->find("PawnKinds", scenario.text("colonistPawnKind"));
    if (!scenario.text("colonistPawnKind").empty() && gearKind == nullptr) {
        setupErrors_.push_back(who + "no PawnKinds def '" + scenario.text("colonistPawnKind") + "'");
        return kNoEntity;
    }
    Colonist colonist;
    std::string error;
    ColonistGenOptions options;
    options.category = gearKind ? gearKind->text("backstoryCategory", "Colony") : "Colony";
    options.traitPoints = scenarioTraitPoints();
    options.noTraits = protagonist && options.traitPoints >= 0;  // who YOU are is cosmetic for now (sneed 2026-11-21)
    options.age = protagonist ? 0 : static_cast<int>(scenario.integer("colonistAge", 0));
    if (!generateColonist(*defs_, colonistSeed, protagonist, colonist, error, options)) {
        setupErrors_.push_back(who + error);
        return kNoEntity;
    }
    makeNameUnique(colonist, colonistSeed);
    const Entity pawn = things_->spawn(kind, cell);
    if (pawn == kNoEntity || !things_->registry().all_of<Mover>(pawn)) {
        things_->destroy(pawn);
        setupErrors_.push_back(who + "'" + kindName + "' is not a pawn");
        return kNoEntity;
    }
    if (Health* health = things_->registry().try_get<Health>(pawn)) {
        health->protagonist = colonist.protagonist;  // 2x part hit points (docs/design/health.md)
    }
    jobs_->addWorker(pawn, colonist.disabledWorkTags);
    giveLooks(pawn, colonistSeed, colonist.female, protagonist ? lineage : scenarioRollLineage(scenario, colonistSeed, lineage));
    things_->registry().emplace<Colonist>(pawn, std::move(colonist));

    // They land dressed, as RimWorld's colonists are (nudists excepted), in what their kind wears;
    // a kind may also carry a weapon, each entry by its own chance from the colonist's seed.
    const Colonist& made = things_->registry().get<Colonist>(pawn);
    const bool nudist = std::any_of(made.traits.begin(), made.traits.end(),
                                    [](const TraitRecord& trait) { return trait.def == "Nudist"; });
    const Def& gearDef = gearKind ? *gearKind : scenario;
    const char* wearList = gearKind ? "apparel" : "colonistApparel";
    Rng gearRng(splitMix64(colonistSeed ^ 0x6765617273ULL), 13);
    for (size_t i = 0; !nudist && i < gearDef.count(wearList); ++i) {
        const std::string base = std::string(wearList) + "." + std::to_string(i);
        if (gearKind != nullptr && gearRng.unit() >= static_cast<float>(gearDef.number((base + ".chance").c_str(), 1.0))) {
            continue;
        }
        const DefId piece = defs_->idOf("Things", gearDef.text((base + ".thing").c_str()));
        const DefId stuff = defs_->idOf("Things", gearDef.text((base + ".stuff").c_str()));
        std::string why;
        const Entity item = piece.valid() ? apparel_->make(piece, stuff, -1, cell, &why) : kNoEntity;
        if (item == kNoEntity || !apparel_->putOn(pawn, item, why)) {
            setupErrors_.push_back(who + "can't dress a colonist in '" + gearDef.text((base + ".thing").c_str()) + "'" +
                                   (why.empty() ? "" : ": " + why));
            return kNoEntity;
        }
    }
    if (gearKind != nullptr && gearKind->count("weapons") > 0 && gearRng.unit() < static_cast<float>(gearKind->number("weaponChance", 1.0))) {
        const std::string gun = gearKind->textAt("weapons", gearRng.below(static_cast<uint32_t>(gearKind->count("weapons"))));
        const Entity item = things_->spawn(defs_->idOf("Things", gun), cell);
        if (item == kNoEntity || !combat_->equip(pawn, item)) {
            things_->destroy(item);
            setupErrors_.push_back(who + "can't arm a colonist with '" + gun + "'");
            return kNoEntity;
        }
    }
    return pawn;
}

// Throws away the colonist in slot `slot` (the player's colonists in the order they were made) and
// makes another from a fresh draw of the main stream, on the same spot. Only before the first step.
bool Sim::rerollColonist(int slot, std::string& error) {
    if (time_.steps != 0) {
        error = "the starting colonists can only be rerolled before the game starts";
        return false;
    }
    const Def* scenario = defs_->find("Scenarios", settings_.scenario);
    if (scenario == nullptr) {
        error = "there is no starting scenario to reroll";
        return false;
    }
    const std::vector<Entity> slots = startingColonistSlots();
    if (slot < 0 || static_cast<size_t>(slot) >= slots.size()) {
        error = "there is no starting colonist " + std::to_string(slot);
        return false;
    }
    const Entity old = slots[static_cast<size_t>(slot)];
    const CellPos cell = cellOf(things_->registry().get<Position>(old));
    const bool protagonist = things_->registry().get<Colonist>(old).protagonist;
    std::vector<Entity> gear;
    if (const Gear* worn = things_->registry().try_get<Gear>(old)) {
        gear = worn->apparel;
        gear.insert(gear.end(), worn->stowed.begin(), worn->stowed.end());
        if (worn->weapon != kNoEntity) {
            gear.push_back(worn->weapon);
        }
    }
    const uint64_t colonistSeed = splitMix64((static_cast<uint64_t>(rng_.next()) << 32) ^ rng_.next());
    combat_->dropGear(old);
    for (const Entity item : gear) {
        things_->destroy(item);
    }
    jobs_->forget(old);
    fighters_->forget(old);
    raids_->forget(old);
    mentalBreaks_->forget(old);
    things_->destroy(old);
    const size_t errorsBefore = setupErrors_.size();
    if (makeScenarioColonist(*scenario, cell, colonistSeed, protagonist) == kNoEntity) {
        error = setupErrors_.size() > errorsBefore ? setupErrors_.back() : "the colonist could not be made";
        return false;
    }
    return true;
}

int Sim::scenarioTraitPoints() const {
    const Def* scenario = defs_->find("Scenarios", settings_.scenario);
    return scenario != nullptr && scenario->has("traitPoints") ? static_cast<int>(scenario->integer("traitPoints", 0)) : -1;
}

// The companions screen's traits (docs/design/new-game.md): a starting colonist's traits, bought from the scenario's
// point pool, before the first step. Skills keep their base and take the new traits' gains; work follows the traits.
bool Sim::setStartingTraits(const SetColonistTraitsCommand& command, std::string& error) {
    if (time_.steps != 0) {
        error = "traits can only be chosen before the game starts";
        return false;
    }
    const int pool = scenarioTraitPoints();
    if (pool < 0) {
        error = "this scenario's people come with backstories, not trait points";
        return false;
    }
    const std::vector<Entity> slots = startingColonistSlots();
    if (command.slot < 0 || static_cast<size_t>(command.slot) >= slots.size()) {
        error = "there is no starting colonist " + std::to_string(command.slot);
        return false;
    }
    const Entity pawn = slots[static_cast<size_t>(command.slot)];
    Colonist& colonist = things_->registry().get<Colonist>(pawn);
    if (colonist.protagonist) {
        error = "who you are has no traits to choose";
        return false;
    }
    std::vector<TraitRecord> traits;
    if (command.randomize) {
        traits = randomTraitSet(*defs_, splitMix64(settings_.seed ^ 0x7472616974726F6CULL ^ (static_cast<uint64_t>(command.slot) << 40) ^
                                                   command.roll),
                                pool);
    } else {
        if (command.traits.size() > 1024) {
            error = "too many traits";
            return false;
        }
        size_t start = 0;
        while (start < command.traits.size()) {
            size_t end = command.traits.find(';', start);
            if (end == std::string::npos) {
                end = command.traits.size();
            }
            const std::string entry = command.traits.substr(start, end - start);
            start = end + 1;
            if (entry.empty()) {
                continue;
            }
            const size_t colon = entry.find(':');
            TraitRecord record;
            record.def = entry.substr(0, colon);
            if (colon != std::string::npos) {
                const std::string degree = entry.substr(colon + 1);
                const bool negative = !degree.empty() && degree[0] == '-';
                if (degree.size() != (negative ? 2u : 1u) || degree.back() < '0' || degree.back() > '9') {
                    error = "bad trait degree in '" + entry + "'";
                    return false;
                }
                record.degree = (degree.back() - '0') * (negative ? -1 : 1);
            }
            traits.push_back(record);
        }
    }
    if (!traitSetFits(*defs_, traits, pool, error)) {
        return false;
    }
    setColonistTraits(*defs_, colonist, traits);
    things_->registry().emplace_or_replace<WorkSettings>(pawn, jobs_->defaultWorkSettings(colonist.disabledWorkTags));
    return true;
}

std::vector<Entity> Sim::startingColonistSlots() const {
    std::vector<Entity> slots;
    for (const auto [pawn, colonist] : things_->registry().view<Colonist>().each()) {
        (void)colonist;
        if (things_->spawned(pawn) && fighters_->factionOf(pawn) == kPlayerFaction) {
            slots.push_back(pawn);
        }
    }
    // By index, not version: a new colonist takes the freed index, so the slots keep their order.
    std::sort(slots.begin(), slots.end(), [](Entity a, Entity b) { return entt::to_entity(a) < entt::to_entity(b); });
    return slots;
}

// The new-game character screen's edits (docs/design/new-game.md): names, gender and the creator's look of a
// starting colonist, before the first step. Everything else about them stays (backstories, traits, skills, gear).
bool Sim::editColonist(const EditColonistCommand& edit, std::string& error) {
    if (time_.steps != 0) {
        error = "the starting colonists can only be edited before the game starts";
        return false;
    }
    const std::vector<Entity> slots = startingColonistSlots();
    if (edit.slot < 0 || static_cast<size_t>(edit.slot) >= slots.size()) {
        error = "there is no starting colonist " + std::to_string(edit.slot);
        return false;
    }
    for (const std::string* name : {&edit.firstName, &edit.nickName, &edit.lastName}) {
        if (name->size() > 24) {
            error = "a name can have at most 24 letters";
            return false;
        }
    }
    if (edit.creator.size() > 8192) {
        error = "the creator's look is too long";
        return false;
    }
    if (edit.female < -1 || edit.female > 1) {
        error = "female must be -1 (keep), 0 or 1";
        return false;
    }
    const Entity pawn = slots[static_cast<size_t>(edit.slot)];
    entt::registry& reg = things_->registry();
    Colonist& colonist = reg.get<Colonist>(pawn);
    if (!edit.firstName.empty()) {
        colonist.firstName = edit.firstName;
    }
    if (!edit.nickName.empty()) {
        colonist.nickName = edit.nickName;
    }
    if (!edit.lastName.empty()) {
        colonist.lastName = edit.lastName;
    }
    if (edit.female >= 0 && (edit.female == 1) != colonist.female) {
        colonist.female = edit.female == 1;
        // New looks for the other gender from a seed of the game, the slot and the gender, so the same edits give the
        // same person; the lineage and the creator's look (if any) stay.
        const PawnLooks* had = reg.try_get<PawnLooks>(pawn);
        const std::string lineage = had != nullptr ? had->lineage : "Baseline";
        const std::string creator = had != nullptr ? had->creator : std::string();
        giveLooks(pawn, splitMix64(settings_.seed ^ 0x6564697447656E64ULL ^ (static_cast<uint64_t>(edit.slot) << 8) ^
                                   static_cast<uint64_t>(edit.female)),
                  colonist.female, lineage);
        if (PawnLooks* looks = reg.try_get<PawnLooks>(pawn)) {
            looks->creator = creator;
        }
    }
    if (edit.setCreator) {
        if (PawnLooks* looks = reg.try_get<PawnLooks>(pawn)) {
            looks->creator = edit.creator;
        }
    }
    return true;
}

// The landing in the clear (sneed 2026-11-25: "make sure that you land in a cleared area, not in the trees"). Crash
// Landed lands in the tropics, where the jungle stands thick: the landing moves to the dry spot within
// `landingSearchM` of the middle with the fewest trees and tall plants round it (a little nearer the middle wins a
// tie), then every tree and tall plant within `landingClearRadiusM` goes, and a ring of 3 m beyond it is thinned, more
// towards the inside, so the clearing's edge is ragged. Grass stays. Its own random stream: nothing else moves.
void Sim::clearLandingSite(const Def& scenario) {
    const double radius = scenario.number("landingClearRadiusM", 0.0);
    if (radius <= 0.0 || !stockpileCell_) {
        return;
    }
    constexpr int kDryRadius = 4;  // as placeStockpileAndThings
    constexpr double kFringeM = 3.0;
    const int reach = static_cast<int>(std::ceil(radius + kFringeM));
    const double radiusSq = radius * radius;
    const auto crowding = [&](CellPos at) {
        int score = 0;
        for (int dy = -reach; dy <= reach; ++dy) {
            for (int dx = -reach; dx <= reach; ++dx) {
                const CellPos c{at.x + dx, at.y + dy, 0};
                if (dx * dx + dy * dy <= radiusSq && map_.inBounds(c) && scenarioClearsPlant(*defs_, *plants_, c)) {
                    score += plants_->isTree(c) ? 3 : 1;  // a tree is the bigger clearing job, and the one that hides you
                }
            }
        }
        return score;
    };
    const int search = std::max(0, static_cast<int>(scenario.integer("landingSearchM", 0)));
    const CellPos middle = *stockpileCell_;
    CellPos best = middle;
    int bestScore = crowding(middle) * 4;
    for (int dy = -search; dy <= search; dy += 2) {
        for (int dx = -search; dx <= search; dx += 2) {
            const CellPos c{middle.x + dx, middle.y + dy, 0};
            if ((dx == 0 && dy == 0) || !map_.walkable(c) || !scenarioDryAround(map_, c, kDryRadius)) {
                continue;
            }
            // Twelve points a tree, four a tall plant, one for every 2 m further from the middle.
            const int score = crowding(c) * 4 + static_cast<int>(std::sqrt(static_cast<double>(dx * dx + dy * dy)) * 0.5);
            if (score < bestScore) {
                bestScore = score;
                best = c;
            }
        }
    }
    stockpileCell_ = best;

    Rng rng(splitMix64(settings_.seed ^ 0x636C656172696E67ULL), 17);
    for (int dy = -reach; dy <= reach; ++dy) {
        for (int dx = -reach; dx <= reach; ++dx) {
            const CellPos c{best.x + dx, best.y + dy, 0};
            if (!map_.inBounds(c) || !scenarioClearsPlant(*defs_, *plants_, c)) {
                continue;
            }
            const double d = std::sqrt(static_cast<double>(dx * dx + dy * dy));
            if (d > radius + kFringeM) {
                continue;
            }
            // Inside: always. The fringe: from a sure clear at its inner edge down to none at its outer one.
            const double keep = d <= radius ? 0.0 : (d - radius) / kFringeM;
            if (d <= radius || rng.unit() >= keep) {
                plants_->remove(c);
            }
        }
    }
}

// Starts the scenario named in the settings (docs/design/colonists.md): the protagonist and the
// colonists made from the seed, standing near the middle of the map, with the scenario's items
// lying around them. Uses its own random streams, so the rest of the sim is not disturbed.
void Sim::startScenario() {
    const Def* scenario = defs_->find("Scenarios", settings_.scenario);
    if (scenario == nullptr) {
        setupErrors_.push_back("no Scenarios def '" + settings_.scenario + "'");
        return;
    }
    if (!stockpileCell_) {
        setupErrors_.push_back("scenario '" + settings_.scenario + "': no walkable ground to land on");
        return;
    }
    const DefId kind = defs_->idOf("Things", scenario->text("colonistKind", "Human"));
    if (!kind.valid()) {
        setupErrors_.push_back("scenario '" + settings_.scenario + "': no Things def '" + scenario->text("colonistKind", "Human") + "'");
        return;
    }

    const std::string lineage = scenario->text("lineage", "Baseline");
    if (!scenario->text("lineage").empty() && defs_->find("Lineages", lineage) == nullptr) {
        setupErrors_.push_back("scenario '" + settings_.scenario + "': no Lineages def '" + lineage + "'");
        return;
    }

    for (const std::string& name : scenario->keys("lineageWeights")) {
        if (defs_->find("Lineages", name) == nullptr) {
            setupErrors_.push_back("scenario '" + settings_.scenario + "': no Lineages def '" + name + "'");
            return;
        }
    }

    // What the colony knows when it lands (docs/design/research.md).
    research_->setNeolithicStart(scenario->flag("neolithicStart"));
    for (size_t i = 0; i < scenario->count("startingResearch"); ++i) {
        std::string error;
        if (!research_->unlock(scenario->textAt("startingResearch", i), error)) {
            setupErrors_.push_back("scenario '" + settings_.scenario + "': " + error);
        }
    }

    clearLandingSite(*scenario);
    const std::vector<CellPos> cells = scenarioNearbyCells(map_, *stockpileCell_, 6000);
    std::set<int> used;
    Rng rng(splitMix64(settings_.seed ^ 0x7363656E6172696FULL), 5);

    // The colonists, two metres apart; the protagonist first.
    const int count = static_cast<int>(scenario->integer("colonists", 0)) + (scenario->flag("protagonist") ? 1 : 0);
    int placed = 0;
    for (const CellPos& cell : cells) {
        if (placed >= count) {
            break;
        }
        if (cell.x % 2 != 0 || cell.y % 2 != 0) {
            continue;
        }
        const bool protagonist = placed == 0 && scenario->flag("protagonist");
        const uint64_t colonistSeed = splitMix64(settings_.seed ^ 0x636F6C6FULL) + static_cast<uint64_t>(placed);
        if (makeScenarioColonist(*scenario, cell, colonistSeed, protagonist) == kNoEntity) {
            return;
        }
        used.insert(map_.index(cell));
        ++placed;
    }
    if (placed < count) {
        setupErrors_.push_back("scenario '" + settings_.scenario + "': no room for all the colonists");
        return;
    }

    // Items as stacks, one stack per free cell: the starting things next to the colonists, the
    // nearby things a little further out, the scattered things anywhere in the area.
    const auto dropStacks = [&](const char* list, size_t firstCell, bool scatter) {
        const size_t entries = scenario->count(list);
        for (size_t i = 0; i < entries; ++i) {
            const std::string base = std::string(list) + "." + std::to_string(i);
            const std::string name = scenario->text((base + ".thing").c_str());
            const DefId item = defs_->idOf("Things", name);
            if (!item.valid()) {
                setupErrors_.push_back("scenario '" + settings_.scenario + "': no Things def '" + name + "'");
                return;
            }
            // Clothes come made of a stuff (Crash Landed's cotton shirts and trousers): one piece per cell.
            const std::string stuffName = scenario->text((base + ".stuff").c_str());
            const DefId stuff = stuffName.empty() ? DefId{} : defs_->idOf("Things", stuffName);
            if (!stuffName.empty() && !stuff.valid()) {
                setupErrors_.push_back("scenario '" + settings_.scenario + "': no Things def '" + stuffName + "'");
                return;
            }
            const int limit = stuff.valid() ? 1 : std::max(1, static_cast<int>(defs_->get(item).integer("stackLimit", 1)));
            for (int left = static_cast<int>(scenario->integer((base + ".count").c_str(), 0)); left > 0; left -= limit) {
                size_t index = firstCell;
                if (scatter) {
                    index = std::min(cells.size() - 1, firstCell + rng.below(static_cast<uint32_t>(cells.size() - firstCell)));
                }
                while (index < cells.size() && used.count(map_.index(cells[index])) != 0) {
                    ++index;
                }
                const Entity made = index >= cells.size() ? kNoEntity
                                    : stuff.valid()       ? apparel_->make(item, stuff, -1, cells[index])
                                                          : things_->spawn(item, cells[index], std::min(left, limit));
                if (made == kNoEntity) {
                    setupErrors_.push_back("scenario '" + settings_.scenario + "': no room for '" + name + "'");
                    return;
                }
                used.insert(map_.index(cells[index]));
            }
        }
    };
    dropStacks("startingThings", 0, false);
    dropStacks("nearbyThings", 40, false);
    dropStacks("scatteredThings", 200, true);
}

// A newcomer who joins the colony (T2's wanderer and crashed survivor): made like the scenario's
// colonists, from `seed`, standing in `cell`, dressed as they are (nudists excepted). kNoEntity if
// the scenario or the cell won't have one.
Entity Sim::joinColonist(CellPos cell, uint64_t seed) {
    const Def* scenario = defs_->find("Scenarios", settings_.scenario);
    if (scenario == nullptr || !map_.walkable(cell)) {
        return kNoEntity;
    }
    const DefId kind = defs_->idOf("Things", scenario->text("colonistKind", "Human"));
    Colonist colonist;
    std::string error;
    ColonistGenOptions options;
    options.traitPoints = scenarioTraitPoints();
    if (!kind.valid() || !generateColonist(*defs_, seed, false, colonist, error, options)) {
        return kNoEntity;
    }
    makeNameUnique(colonist, seed);
    const Entity pawn = things_->spawn(kind, cell);
    if (pawn == kNoEntity || !things_->registry().all_of<Mover>(pawn)) {
        things_->destroy(pawn);
        return kNoEntity;
    }
    jobs_->addWorker(pawn, colonist.disabledWorkTags);
    giveLooks(pawn, seed, colonist.female, scenarioRollLineage(*scenario, seed, scenario->text("lineage", "Baseline")));
    things_->registry().emplace<Colonist>(pawn, std::move(colonist));
    const Colonist& made = things_->registry().get<Colonist>(pawn);
    const bool nudist = std::any_of(made.traits.begin(), made.traits.end(),
                                    [](const TraitRecord& trait) { return trait.def == "Nudist"; });
    for (size_t i = 0; !nudist && i < scenario->count("colonistApparel"); ++i) {
        const std::string base = "colonistApparel." + std::to_string(i);
        const DefId piece = defs_->idOf("Things", scenario->text((base + ".thing").c_str()));
        const DefId stuff = defs_->idOf("Things", scenario->text((base + ".stuff").c_str()));
        std::string why;
        const Entity item = piece.valid() ? apparel_->make(piece, stuff, -1, cell, &why) : kNoEntity;
        if (item != kNoEntity && !apparel_->putOn(pawn, item, why)) {
            things_->destroy(item);
        }
    }
    return pawn;
}

}  // namespace colony

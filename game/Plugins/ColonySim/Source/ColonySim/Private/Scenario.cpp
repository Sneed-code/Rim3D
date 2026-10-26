#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Looks.h"
#include "ColonySim/Random.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"

#include <algorithm>
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

}  // namespace

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
        Colonist colonist;
        std::string error;
        const bool protagonist = placed == 0 && scenario->flag("protagonist");
        const uint64_t colonistSeed = splitMix64(settings_.seed ^ 0x636F6C6FULL) + static_cast<uint64_t>(placed);
        if (!generateColonist(*defs_, colonistSeed, protagonist, colonist, error)) {
            setupErrors_.push_back("scenario '" + settings_.scenario + "': " + error);
            return;
        }
        const Entity pawn = things_->spawn(kind, cell);
        if (pawn == kNoEntity || !things_->registry().all_of<Mover>(pawn)) {
            things_->destroy(pawn);
            setupErrors_.push_back("scenario '" + settings_.scenario + "': '" + scenario->text("colonistKind", "Human") + "' is not a pawn");
            return;
        }
        if (Health* health = things_->registry().try_get<Health>(pawn)) {
            health->protagonist = colonist.protagonist;  // 2x part hit points (docs/design/health.md)
        }
        jobs_->addWorker(pawn, colonist.disabledWorkTags);
        giveLooks(pawn, colonistSeed, colonist.female,
                  protagonist ? lineage : scenarioRollLineage(*scenario, colonistSeed, lineage));
        things_->registry().emplace<Colonist>(pawn, std::move(colonist));
        // They land dressed, as RimWorld's colonists are (nudists excepted).
        const Colonist& made = things_->registry().get<Colonist>(pawn);
        const bool nudist = std::any_of(made.traits.begin(), made.traits.end(),
                                        [](const TraitRecord& trait) { return trait.def == "Nudist"; });
        for (size_t i = 0; !nudist && i < scenario->count("colonistApparel"); ++i) {
            const std::string base = "colonistApparel." + std::to_string(i);
            const DefId piece = defs_->idOf("Things", scenario->text((base + ".thing").c_str()));
            const DefId stuff = defs_->idOf("Things", scenario->text((base + ".stuff").c_str()));
            std::string why;
            const Entity item = piece.valid() ? apparel_->make(piece, stuff, -1, cell, &why) : kNoEntity;
            if (item == kNoEntity || !apparel_->putOn(pawn, item, why)) {
                setupErrors_.push_back("scenario '" + settings_.scenario + "': can't dress a colonist in '" +
                                       scenario->text((base + ".thing").c_str()) + "'" + (why.empty() ? "" : ": " + why));
                return;
            }
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
            const int limit = std::max(1, static_cast<int>(defs_->get(item).integer("stackLimit", 1)));
            for (int left = static_cast<int>(scenario->integer((base + ".count").c_str(), 0)); left > 0; left -= limit) {
                size_t index = firstCell;
                if (scatter) {
                    index = std::min(cells.size() - 1, firstCell + rng.below(static_cast<uint32_t>(cells.size() - firstCell)));
                }
                while (index < cells.size() && used.count(map_.index(cells[index])) != 0) {
                    ++index;
                }
                if (index >= cells.size() ||
                    things_->spawn(item, cells[index], std::min(left, limit)) == kNoEntity) {
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
    if (!kind.valid() || !generateColonist(*defs_, seed, false, colonist, error)) {
        return kNoEntity;
    }
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

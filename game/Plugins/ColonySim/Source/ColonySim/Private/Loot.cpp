#include "ColonySim/Loot.h"

#include "ColonySim/Map.h"
#include "ColonySim/Raids.h"
#include "ColonySim/Things.h"

#include <algorithm>
#include <cmath>

namespace colony {

namespace {

constexpr uint64_t kLootSalt = 0x6C6F6F7473ULL;  // "loots"

}  // namespace

LootNumbers LootNumbers::read(const DefDatabase& defs) {
    LootNumbers n;
    const Def* def = defs.find("Rules", "LootRules");
    if (def == nullptr) {
        return n;
    }
    n.referencePoints = static_cast<float>(def->number("referencePoints", n.referencePoints));
    n.minScale = static_cast<float>(def->number("minScale", n.minScale));
    n.maxScale = static_cast<float>(def->number("maxScale", n.maxScale));
    n.dropRadius = static_cast<int>(def->integer("dropRadius", n.dropRadius));
    n.podMinDistance = static_cast<int>(def->integer("podMinDistance", n.podMinDistance));
    n.podMaxDistance = static_cast<int>(def->integer("podMaxDistance", n.podMaxDistance));
    return n;
}

LootSystem::LootSystem(Things& things, Map& map, const DefDatabase& defs, uint64_t seed)
    : things_(things), map_(map), defs_(defs), seed_(seed), numbers_(LootNumbers::read(defs)) {
    const auto readEntry = [&](const Def& def, const std::string& base) {
        Entry e;
        e.thing = defs.idOf("Things", def.text((base + ".thing").c_str()));
        e.chance = static_cast<float>(def.number((base + ".chance").c_str(), 1.0));
        e.weight = static_cast<float>(def.number((base + ".weight").c_str(), 1.0));
        e.perHundredPoints = static_cast<float>(def.number((base + ".perHundredPoints").c_str(), 0.0));
        if (def.count((base + ".count").c_str()) == 2) {
            e.hasCount = true;
            e.countMin = std::max(1, static_cast<int>(def.integer((base + ".count.0").c_str(), 1)));
            e.countMax = std::max(e.countMin, static_cast<int>(def.integer((base + ".count.1").c_str(), e.countMin)));
        }
        return e;
    };
    for (const DefId id : defs.idsOfKind("LootTables")) {
        const Def& def = defs.get(id);
        Table t;
        t.name = def.defName();
        for (size_t i = 0; i < def.count("items"); ++i) {
            const Entry e = readEntry(def, "items." + std::to_string(i));
            if (e.thing.valid()) {
                t.items.push_back(e);
            }
        }
        if (def.has("stacks")) {
            t.rollsMin = static_cast<int>(def.integer("stacks.rolls.0", 1));
            t.rollsMax = std::max(t.rollsMin, static_cast<int>(def.integer("stacks.rolls.1", t.rollsMin)));
            t.valueLo = static_cast<float>(def.number("stacks.value.0", 0.0));
            t.valueHi = std::max(t.valueLo, static_cast<float>(def.number("stacks.value.1", t.valueLo)));
            t.pointsScaled = def.flag("stacks.pointsScaled");
            for (size_t i = 0; i < def.count("stacks.from"); ++i) {
                const Entry e = readEntry(def, "stacks.from." + std::to_string(i));
                if (e.thing.valid()) {
                    t.from.push_back(e);
                }
            }
        }
        tables_.push_back(std::move(t));
    }
}

const LootSystem::Table* LootSystem::find(const std::string& name) const {
    for (const Table& t : tables_) {
        if (t.name == name) {
            return &t;
        }
    }
    return nullptr;
}

bool LootSystem::hasTable(const std::string& table) const {
    return find(table) != nullptr;
}

int LootSystem::stackLimit(DefId def) const {
    return std::max(1, static_cast<int>(defs_.get(def).integer("stackLimit", 1)));
}

Rng LootSystem::rngFor(uint64_t salt, int64_t second) const {
    return Rng(splitMix64(seed_ ^ kLootSalt ^ (salt << 20) ^ static_cast<uint64_t>(second)));
}

std::string LootSystem::tableOfKind(const std::string& pawnKind) const {
    const Def* kind = defs_.find("PawnKinds", pawnKind);
    return kind != nullptr ? kind->text("lootTable") : std::string();
}

std::vector<LootItem> LootSystem::roll(const std::string& table, float points, Rng& rng) const {
    std::vector<LootItem> out;
    const Table* t = find(table);
    if (t == nullptr) {
        return out;
    }
    float scale = 1.0f;
    if (points > 0.0f && numbers_.referencePoints > 0.0f) {
        scale = std::clamp(points / numbers_.referencePoints, numbers_.minScale, numbers_.maxScale);
    }
    const auto add = [&out](DefId def, int count) {
        if (count <= 0) {
            return;
        }
        for (LootItem& item : out) {
            if (item.def == def) {
                item.count += count;
                return;
            }
        }
        out.push_back(LootItem{def, count});
    };
    for (const Entry& e : t->items) {
        if (rng.unit() >= e.chance) {
            continue;
        }
        int count = e.countMin + static_cast<int>(rng.below(static_cast<uint32_t>(e.countMax - e.countMin + 1)));
        if (e.perHundredPoints > 0.0f && points > 0.0f) {
            count += static_cast<int>(std::floor(e.perHundredPoints * points / 100.0f));
        }
        add(e.thing, count);
    }
    if (!t->from.empty() && t->rollsMax > 0) {
        float total = 0.0f;
        for (const Entry& e : t->from) {
            total += e.weight;
        }
        const int rolls = t->rollsMin + static_cast<int>(rng.below(static_cast<uint32_t>(t->rollsMax - t->rollsMin + 1)));
        for (int r = 0; r < rolls && total > 0.0f; ++r) {
            float pick = rng.unit() * total;
            const Entry* chosen = &t->from.back();
            for (const Entry& e : t->from) {
                if (pick < e.weight) {
                    chosen = &e;
                    break;
                }
                pick -= e.weight;
            }
            const Def& def = defs_.get(chosen->thing);
            int count = 1;
            if (chosen->hasCount) {
                count = chosen->countMin + static_cast<int>(rng.below(static_cast<uint32_t>(chosen->countMax - chosen->countMin + 1)));
            } else {
                const float value = rng.range(t->valueLo, t->valueHi) * (t->pointsScaled ? scale : 1.0f);
                const float each = static_cast<float>(def.number("stats.marketValue", 0.0));
                count = each > 0.0f ? static_cast<int>(std::lround(value / each)) : 1;
                count = std::clamp(count, 1, stackLimit(chosen->thing));
            }
            add(chosen->thing, count);
        }
    }
    return out;
}

bool LootSystem::freeCell(CellPos cell) const {
    return map_.walkable(cell) && things_.itemsAt(cell).empty();
}

std::vector<Entity> LootSystem::drop(const std::vector<LootItem>& items, CellPos cell) {
    std::vector<Entity> made;
    // Free cells nearest first, by rings round the cell.
    std::vector<CellPos> free;
    const int radius = std::max(1, numbers_.dropRadius);
    for (int ring = 0; ring <= radius; ++ring) {
        for (int dy = -ring; dy <= ring; ++dy) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring) {
                    continue;
                }
                const CellPos c{cell.x + dx, cell.y + dy, cell.level};
                if (freeCell(c)) {
                    free.push_back(c);
                }
            }
        }
    }
    size_t next = 0;
    for (const LootItem& item : items) {
        int left = item.count;
        const int limit = stackLimit(item.def);
        while (left > 0) {
            const int n = std::min(left, limit);
            const CellPos at = next < free.size() ? free[next++] : cell;  // no room: pile on the spot
            const Entity thing = things_.spawn(item.def, at, n);
            if (thing != kNoEntity) {
                made.push_back(thing);
            }
            left -= n;
        }
    }
    return made;
}

bool LootSystem::cargoPod(const std::string& table, CellPos centre, Rng& rng, CellPos& where, std::vector<LootItem>& items) {
    if (!hasTable(table)) {
        return false;
    }
    const int lo = std::max(0, numbers_.podMinDistance);
    const int hi = std::max(lo + 1, numbers_.podMaxDistance);
    for (int attempt = 0; attempt < 200; ++attempt) {
        const float angle = rng.range(0.0f, 6.2831853f);
        const float dist = rng.range(static_cast<float>(lo), static_cast<float>(hi));
        const CellPos c{centre.x + static_cast<int>(std::lround(std::cos(angle) * dist)),
                        centre.y + static_cast<int>(std::lround(std::sin(angle) * dist)), 0};
        if (map_.onGround(c) && freeCell(c) && map_.roof(c) == Roof::None) {
            where = c;
            items = roll(table, 0.0f, rng);
            drop(items, c);
            return true;
        }
    }
    return false;
}

void LootSystem::step(const std::vector<Event>& events, size_t from, const RaidState& raids, int64_t second) {
    entt::registry& reg = things_.registry();
    for (size_t i = from; i < events.size(); ++i) {
        if (events[i].type != EventType::PawnDied) {
            continue;
        }
        const Entity pawn = events[i].thing;
        if (!reg.valid(pawn) || !reg.all_of<Raider, Position>(pawn)) {
            continue;
        }
        const Raider& raider = reg.get<Raider>(pawn);
        const std::string table = tableOfKind(raider.kind);
        if (table.empty()) {
            continue;
        }
        float points = 0.0f;
        for (const Squad& squad : raids.squads) {
            if (squad.id == raider.squad) {
                points = squad.points;
                break;
            }
        }
        Rng rng = rngFor(entt::to_integral(pawn), second);
        const std::vector<LootItem> items = roll(table, points, rng);
        drop(items, cellOf(reg.get<Position>(pawn)));
    }
}

std::string LootSystem::describe(const std::vector<LootItem>& items) const {
    std::string out;
    for (const LootItem& item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += std::to_string(item.count) + " " + defs_.get(item.def).label();
    }
    return out;
}

}  // namespace colony

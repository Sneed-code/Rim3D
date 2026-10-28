// A settlement on its map (W6, docs/design/settlements.md): when a caravan first walks onto a settlement's tile, its
// map gets the faction's base (walled rooms round a yard, furniture, a wall round it for industrial factions) and its
// people (TradeSystem::startSettlement): a trader to deal with, guards and villagers, or defenders if the faction is
// the colony's enemy. Built once; the map keeps it like anything else on it.
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Fuel.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/OtherMaps.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Random.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Things.h"
#include "ColonySim/Trade.h"
#include "ColonySim/World.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace colony {

namespace {

std::vector<std::string> settlementTexts(const Def& def, const std::string& path) {
    std::vector<std::string> out;
    for (size_t i = 0; i < def.count(path.c_str()); ++i) {
        out.push_back(def.textAt(path.c_str(), i));
    }
    return out;
}

// The size of a def's footprint facing North (its `size`, 1 by 1 without one).
void settlementSize(const Def& def, int& sx, int& sy) {
    sx = std::max(1, static_cast<int>(def.numberAt("size", 0, 1.0)));
    sy = std::max(1, static_cast<int>(def.numberAt("size", 1, 1.0)));
}

}  // namespace

SettlementRules SettlementRules::read(const DefDatabase& defs) {
    SettlementRules r;
    const Def* rules = defs.find("Rules", "SettlementRules");
    if (rules == nullptr) {
        return r;
    }
    r.baseSizeMin = std::clamp(static_cast<int>(rules->integer("baseSize.0", r.baseSizeMin)), 12, 200);
    r.baseSizeMax = std::clamp(static_cast<int>(rules->integer("baseSize.1", r.baseSizeMax)), r.baseSizeMin, 200);
    r.residentsMin = std::clamp(static_cast<int>(rules->integer("residents.0", r.residentsMin)), 1, 100);
    r.residentsMax = std::clamp(static_cast<int>(rules->integer("residents.1", r.residentsMax)), r.residentsMin, 100);
    r.blocks = std::clamp(static_cast<int>(rules->integer("blocks", r.blocks)), 1, 7);
    r.roomChance = static_cast<float>(rules->number("roomChance", r.roomChance));
    for (const std::string& tech : rules->keys("wallStuff")) {
        r.wallStuff[tech] = settlementTexts(*rules, "wallStuff." + tech);
    }
    for (const std::string& tech : rules->keys("furnitureStuff")) {
        r.furnitureStuff[tech] = settlementTexts(*rules, "furnitureStuff." + tech);
    }
    r.perimeterTechLevels = settlementTexts(*rules, "perimeterTechLevels");
    for (size_t i = 0; i < rules->count("rooms"); ++i) {
        const std::string at = "rooms." + std::to_string(i);
        Room room;
        room.kind = rules->text((at + ".kind").c_str());
        room.weight = std::max(0.0f, static_cast<float>(rules->number((at + ".weight").c_str(), 1.0)));
        room.beds = rules->flag((at + ".beds").c_str());
        room.things = settlementTexts(*rules, at + ".things");
        r.rooms.push_back(std::move(room));
    }
    r.yard = settlementTexts(*rules, "yard");
    return r;
}

bool Sim::buildSettlement(std::string& error) {
    if (world_ == nullptr || !settings_.world.visit) {
        error = "only another map has a settlement";
        return false;
    }
    const int tile = settings_.world.startTile;
    if (tile < 0 || tile >= world_->tileCount() || world_->tile(tile).settlement == kNoSettlement) {
        return true;  // no settlement here: nothing to build
    }
    const WorldSettlement& home = world_->settlements()[static_cast<size_t>(world_->tile(tile).settlement)];
    const Def* faction = defs_->find("Factions", home.faction);
    if (faction == nullptr) {
        error = "no Factions def '" + home.faction + "'";
        return false;
    }
    const SettlementRules rules = SettlementRules::read(*defs_);
    // The base's own stream, from the world and the tile: the same settlement is always laid out the same way.
    Rng rng(splitMix64(settings_.world.seed ^ 0x736574746C656D65ULL ^ (static_cast<uint64_t>(tile) << 20)), 11);
    entt::registry& reg = things_->registry();
    const int w = map_.width();
    const int h = map_.height();

    // ---- Where: the most open square, nearest the middle -----------------------------------------------------------
    int size = rules.baseSizeMin + static_cast<int>(rng.below(static_cast<uint32_t>(rules.baseSizeMax - rules.baseSizeMin + 1)));
    const int margin = 8;  // cells kept free at the map's edge, where caravans walk on
    size = std::min(size, std::min(w, h) - 2 * margin);
    if (size < 12) {
        error = "the map is too small for a settlement";
        return false;
    }
    std::vector<int> open(static_cast<size_t>((w + 1) * (h + 1)), 0);  // prefix sums of buildable cells
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const CellPos c{x, y, 0};
            const int here = map_.groundWalkable(c) && !mineables_->has(c) && map_.buildingAt(c) == kNoEntity ? 1 : 0;
            open[static_cast<size_t>((y + 1) * (w + 1) + x + 1)] = here + open[static_cast<size_t>(y * (w + 1) + x + 1)] +
                                                                    open[static_cast<size_t>((y + 1) * (w + 1) + x)] -
                                                                    open[static_cast<size_t>(y * (w + 1) + x)];
        }
    }
    const auto openIn = [&](int x0, int y0) {
        const auto at = [&](int x, int y) { return open[static_cast<size_t>(y * (w + 1) + x)]; };
        return at(x0 + size, y0 + size) - at(x0, y0 + size) - at(x0 + size, y0) + at(x0, y0);
    };
    int bx = (w - size) / 2;
    int by = (h - size) / 2;
    int bestOpen = -1;
    int64_t bestDist = 0;
    for (int y0 = margin; y0 + size <= h - margin; y0 += 2) {
        for (int x0 = margin; x0 + size <= w - margin; x0 += 2) {
            const int n = openIn(x0, y0);
            const int64_t dx = 2 * x0 + size - w;
            const int64_t dy = 2 * y0 + size - h;
            const int64_t dist = dx * dx + dy * dy;
            if (n > bestOpen || (n == bestOpen && dist < bestDist)) {
                bestOpen = n;
                bestDist = dist;
                bx = x0;
                by = y0;
            }
        }
    }
    for (int y = by; y < by + size; ++y) {
        for (int x = bx; x < bx + size; ++x) {
            plants_->remove(CellPos{x, y, 0});  // the base is cleared ground
        }
    }

    // ---- What of: a stuff its tech level allows ---------------------------------------------------------------------
    const std::string tech = faction->text("techLevel", "Industrial");
    const DefId wallDef = defs_->idOf("Things", "Wall");
    const auto pickStuff = [&](const std::map<std::string, std::vector<std::string>>& table, DefId forDef) {
        std::vector<DefId> fits;
        const auto it = table.find(tech);
        if (it != table.end()) {
            for (const std::string& name : it->second) {
                const DefId stuff = defs_->idOf("Things", name);
                if (stuff.valid() && (!forDef.valid() || construction_->stuffFits(forDef, stuff))) {
                    fits.push_back(stuff);
                }
            }
        }
        if (fits.empty()) {
            fits.push_back(defs_->idOf("Things", "WoodLog"));
        }
        return fits[rng.below(static_cast<uint32_t>(fits.size()))];
    };
    const DefId wallStuff = pickStuff(rules.wallStuff, wallDef);
    const DefId furnitureStuff = pickStuff(rules.furnitureStuff, DefId{});

    // Builds a thing at once, as if colonists had brought everything and done the work; kNoEntity if it can't stand there.
    const auto build = [&](const std::string& defName, DefId stuff, CellPos cell) -> Entity {
        const DefId def = defs_->idOf("Things", defName);
        if (!def.valid() || !construction_->buildable(def)) {
            return kNoEntity;
        }
        DefId use = stuff;
        if (!construction_->stuffFits(def, use)) {
            use = DefId{};
            if (!construction_->stuffFits(def, use)) {
                use = defs_->idOf("Things", "WoodLog");
                if (!construction_->stuffFits(def, use)) {
                    return kNoEntity;
                }
            }
        }
        std::string why;
        const Entity site = construction_->placeBlueprint(def, use, cell, Rot4::North, why);
        if (site == kNoEntity) {
            return kNoEntity;
        }
        for (const Construction::Cost& cost : construction_->costOf(def, use)) {
            construction_->deliver(site, cost.item, cost.count);
        }
        const Entity built = construction_->complete(site);
        if (built != kNoEntity) {
            if (Fuel* fuel = reg.try_get<Fuel>(built)) {
                fuel->amount = readFuelSpec(*defs_, defs_->get(def)).capacity;  // its people keep it lit
            }
        }
        return built;
    };
    // Whether a def's footprint at `cell` stays inside `inside` and off the `keep` cells.
    const auto fitsIn = [&](const std::string& defName, CellPos cell, const CellRect& inside, const std::vector<CellPos>& keep) {
        const Def* def = defs_->find("Things", defName);
        if (def == nullptr) {
            return false;
        }
        int sx = 1;
        int sy = 1;
        settlementSize(*def, sx, sy);
        const CellRect rect{cell.x, cell.y, sx, sy, 0};
        if (rect.minX < inside.minX || rect.minY < inside.minY || rect.minX + sx > inside.minX + inside.sizeX ||
            rect.minY + sy > inside.minY + inside.sizeY) {
            return false;
        }
        return std::none_of(keep.begin(), keep.end(), [&](const CellPos& k) { return rect.contains(k); });
    };
    // Puts a thing at the first cell of `inside` (row by row from its low corner) where it fits.
    const auto furnish = [&](const std::string& defName, const CellRect& inside, const std::vector<CellPos>& keep) {
        for (int y = inside.minY; y < inside.minY + inside.sizeY; ++y) {
            for (int x = inside.minX; x < inside.minX + inside.sizeX; ++x) {
                const CellPos c{x, y, 0};
                if (fitsIn(defName, c, inside, keep) && build(defName, furnitureStuff, c) != kNoEntity) {
                    return;
                }
            }
        }
    };

    // ---- The wall round it (industrial and spacer factions), a gap in the middle of each side -------------------------
    const bool perimeter = std::find(rules.perimeterTechLevels.begin(), rules.perimeterTechLevels.end(), tech) != rules.perimeterTechLevels.end();
    if (perimeter) {
        const int mid = size / 2;
        for (int i = 0; i < size; ++i) {
            if (std::abs(i - mid) <= 1) {
                continue;  // the gates
            }
            const CellPos sides[4] = {{bx + i, by, 0}, {bx + i, by + size - 1, 0}, {bx, by + i, 0}, {bx + size - 1, by + i, 0}};
            for (const CellPos& c : sides) {
                if (map_.buildingAt(c) == kNoEntity) {
                    build("Wall", wallStuff, c);
                }
            }
        }
    }

    // ---- The rooms, blocks x blocks round the yard ------------------------------------------------------------------------
    const int inner = size - 4;
    const int block = inner / rules.blocks;
    const int middle = rules.blocks / 2;
    float roomWeights = 0.0f;
    for (const SettlementRules::Room& room : rules.rooms) {
        roomWeights += room.weight;
    }
    for (int gy = 0; gy < rules.blocks; ++gy) {
        for (int gx = 0; gx < rules.blocks; ++gx) {
            if ((gx == middle && gy == middle) || block < 5 || rules.rooms.empty() || roomWeights <= 0.0f || rng.unit() >= rules.roomChance) {
                continue;
            }
            float pick = rng.unit() * roomWeights;
            const SettlementRules::Room* kind = &rules.rooms.back();
            for (const SettlementRules::Room& room : rules.rooms) {
                if (pick < room.weight) {
                    kind = &room;
                    break;
                }
                pick -= room.weight;
            }
            // The room is its block less a lane of one cell all round; the walls are its edge.
            const CellRect room{bx + 2 + gx * block + 1, by + 2 + gy * block + 1, block - 2, block - 2, 0};
            const CellRect inside{room.minX + 1, room.minY + 1, room.sizeX - 2, room.sizeY - 2, 0};
            // The door faces the yard (a corner room picks one of its two sides toward it).
            const int sx = gx < middle ? 1 : gx > middle ? -1 : 0;
            const int sy = gy < middle ? 1 : gy > middle ? -1 : 0;
            const bool alongX = sx != 0 && (sy == 0 || rng.below(2) == 0);
            CellPos door;
            CellPos doorInside;
            if (alongX) {
                door = CellPos{sx > 0 ? room.minX + room.sizeX - 1 : room.minX, room.minY + room.sizeY / 2, 0};
                doorInside = CellPos{door.x - sx, door.y, 0};
            } else {
                const int dir = sy != 0 ? sy : 1;
                door = CellPos{room.minX + room.sizeX / 2, dir > 0 ? room.minY + room.sizeY - 1 : room.minY, 0};
                doorInside = CellPos{door.x, door.y - dir, 0};
            }
            for (int y = room.minY; y < room.minY + room.sizeY; ++y) {
                for (int x = room.minX; x < room.minX + room.sizeX; ++x) {
                    const CellPos c{x, y, 0};
                    if (inside.contains(c) || map_.buildingAt(c) != kNoEntity) {
                        continue;
                    }
                    build(c.x == door.x && c.y == door.y ? "Door" : "Wall", wallStuff, c);
                }
            }
            const std::vector<CellPos> keep{doorInside};
            if (kind->beds) {
                // Beds side by side along the low and high walls, a cell apart.
                for (const int row : {inside.minY, inside.minY + inside.sizeY - 2}) {
                    for (int x = inside.minX; x < inside.minX + inside.sizeX; x += 2) {
                        const CellPos c{x, row, 0};
                        if (fitsIn("Bed", c, inside, keep)) {
                            build("Bed", furnitureStuff, c);
                        }
                    }
                    if (inside.sizeY < 4) {
                        break;
                    }
                }
            }
            for (const std::string& thing : kind->things) {
                furnish(thing, inside, keep);
            }
        }
    }

    // ---- The yard: the trader's spot in the middle, a campfire beside it ---------------------------------------------------
    const CellPos spot{bx + size / 2, by + size / 2, 0};
    const CellRect yard{bx + 2 + middle * block, by + 2 + middle * block, block, block, 0};
    std::vector<CellPos> keep{spot};
    for (const std::string& thing : rules.yard) {
        bool placed = false;
        for (int r = 2; r <= block / 2 && !placed; ++r) {
            for (const CellPos c : {CellPos{spot.x + r, spot.y, 0}, CellPos{spot.x - r, spot.y, 0}, CellPos{spot.x, spot.y + r, 0},
                                    CellPos{spot.x, spot.y - r, 0}}) {
                if (fitsIn(thing, c, yard, keep) && build(thing, furnitureStuff, c) != kNoEntity) {
                    keep.push_back(c);
                    placed = true;
                    break;
                }
            }
        }
    }

    // ---- Its people ----------------------------------------------------------------------------------------------------
    const int residents =
        rules.residentsMin + static_cast<int>(rng.below(static_cast<uint32_t>(rules.residentsMax - rules.residentsMin + 1)));
    CellPos middleCell = spot;
    if (!map_.walkable(middleCell)) {
        for (int r = 1; r < size / 2 && !map_.walkable(middleCell); ++r) {
            for (const CellPos c : {CellPos{spot.x + r, spot.y, 0}, CellPos{spot.x - r, spot.y, 0}, CellPos{spot.x, spot.y + r, 0},
                                    CellPos{spot.x, spot.y - r, 0}}) {
                if (map_.walkable(c)) {
                    middleCell = c;
                    break;
                }
            }
        }
    }
    uint32_t visit = 0;
    std::string why;
    // A faction with nobody to send (the old crews) leaves its base empty.
    trade_->startSettlement(home.faction, middleCell, size / 2, residents, visit, why);
    return true;
}

}  // namespace colony

#pragma once

// Small Things defs for sim tests, so tests don't depend on what the RimWorld import contains.
// The numbers are test fixtures, not game numbers.

#include "ColonySim/Defs.h"

#include <memory>
#include <string>
#include <vector>

namespace colony {

inline std::shared_ptr<const DefDatabase> testDefs() {
    const std::vector<DefSource> sources = {{"Things", "test.json", R"([
        {"defName": "Human", "category": "Pawn", "stats": {"moveSpeed": 0.15333}},
        {"defName": "Steel", "category": "Item", "stackLimit": 75, "stats": {"maxHitPoints": 100}},
        {"defName": "WoodLog", "category": "Item", "stackLimit": 75},
        {"defName": "Wall", "category": "Building", "passability": "Impassable", "stats": {"maxHitPoints": 300}},
        {"defName": "Door", "category": "Building", "thingClass": "Building_Door", "passability": "PassThroughOnly"},
        {"defName": "Bed", "category": "Building", "size": [1, 2], "passability": "Standable"},
        {"defName": "FueledStove", "category": "Building", "size": [3, 1], "passability": "PassThroughOnly"},
        {"defName": "Plant_Grass", "category": "Plant"}
    ])"},
                                            {"Terrain", "test.json", R"([{"defName": "Soil"}])"}};
    auto db = std::make_shared<DefDatabase>();
    std::vector<std::string> errors;
    DefDatabase::build(sources, "", *db, errors);
    return db;
}

}  // namespace colony

#include "ColonySim/Defs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

// Models batch 11 (assets/codex_art.json): every def names its own model, meat, leather and wool through their
// abstract bases, and the stone chunks keep their tint (used by the shapes drawn before the model is imported).
TEST_CASE("models batch 11: defs name their Codex and Neon models") {
    DefDatabase db;
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    if (!ok) { CHECK(false); return; }

    const auto meshOf = [&db](const char* name) {
        const Def* d = db.find("Things", name);
        return d != nullptr ? d->text("visual.mesh") : std::string("missing def");
    };
    const auto model = [](const std::string& m) { return "/Game/Rim3D/Generated/Buildings/" + m + "/SM_" + m + ".SM_" + m; };

    for (const char* name : {"Door", "Stairs", "Ladder", "MealSimple", "MealFine", "WoodLog", "RawBerries", "MilkRaw",
                             "ChunkGranite", "ChunkLimestone", "ChunkSandstone", "ChunkSlate", "ChunkMarble",
                             "BlocksGranite", "BlocksLimestone", "BlocksSandstone", "BlocksSlate", "BlocksMarble"}) {
        CHECK_MESSAGE(meshOf(name) == model(name), name);
    }
    CHECK(meshOf("Meat_Cow") == model("Meat"));
    CHECK(meshOf("Meat_Human") == model("Meat"));
    CHECK(meshOf("Leather_Plain") == model("Leather"));
    CHECK(meshOf("WoolSheep") == model("Wool"));
    CHECK(meshOf("EggChickenFertilized") == model("EggSmall"));
    CHECK(meshOf("EggCassowaryFertilized") == model("EggLarge"));
    CHECK(db.find("Things", "ChunkGranite")->count("visual.tint") == 3);
    CHECK(db.find("Things", "ChunkGranite")->count("visual.parts") == 1);
}

#include "doctest.h"

#include "ColonySim/Defs.h"

#include <string>
#include <vector>

using namespace colony;

namespace {

bool defsTestBuild(const std::vector<DefSource>& src, const std::string& tuning, DefDatabase& db,
                   std::vector<std::string>& errors) {
    return DefDatabase::build(src, tuning, db, errors);
}

bool defsTestHasError(const std::vector<std::string>& errors, const std::string& needle) {
    for (const std::string& e : errors) {
        if (e.find(needle) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("defs: loads, resolves parents and skips abstract defs") {
    const std::vector<DefSource> src = {{"Things", "a.json", R"([
        {"defName":"Base","abstract":true,"stats":{"hp":100,"flammability":1.0},"label":"base"},
        {"defName":"Wall","parent":"Base","label":"wall","stats":{"hp":300},"tags":["a","b"]},
        {"defName":"Door","parent":"Wall","stats":{"hp":150}}
    ])"}};
    DefDatabase db;
    std::vector<std::string> errors;
    if (!(defsTestBuild(src, "", db, errors))) { CHECK(false); return; }
    CHECK(errors.empty());
    CHECK(db.size() == 2);
    CHECK(db.find("Things", "Base") == nullptr);
    const Def* wall = db.find("Things", "Wall");
    if (!(wall != nullptr)) { CHECK(false); return; }
    CHECK(wall->integer("stats.hp") == 300);
    CHECK(wall->number("stats.flammability") == doctest::Approx(1.0));
    CHECK(wall->label() == "wall");
    CHECK(wall->count("tags") == 2);
    CHECK(wall->textAt("tags", 1) == "b");
    const Def* door = db.find("Things", "Door");
    if (!(door != nullptr)) { CHECK(false); return; }
    CHECK(door->integer("stats.hp") == 150);
    CHECK(door->number("stats.flammability") == doctest::Approx(1.0));
    CHECK(door->label() == "wall");  // inherited
    CHECK(door->text("abstract", "none") == "none");  // abstract flag is not inherited
    CHECK(db.get(db.idOf("Things", "Door")).defName() == "Door");
}

TEST_CASE("defs: ids are dense and independent of the order files are listed") {
    const DefSource a{"Zed", "z.json", R"([{"defName":"Z1"}])"};
    const DefSource b{"Alpha", "b.json", R"([{"defName":"B1"},{"defName":"B2"}])"};
    const DefSource c{"Alpha", "a.json", R"([{"defName":"A1"}])"};
    DefDatabase one, two;
    std::vector<std::string> errors;
    if (!(defsTestBuild({a, b, c}, "", one, errors))) { CHECK(false); return; }
    if (!(defsTestBuild({c, a, b}, "", two, errors))) { CHECK(false); return; }
    if (!(one.size() == 4)) { CHECK(false); return; }
    CHECK(one.idOf("Alpha", "A1").value == 0);
    CHECK(one.idOf("Alpha", "B1").value == 1);
    CHECK(one.idOf("Alpha", "B2").value == 2);
    CHECK(one.idOf("Zed", "Z1").value == 3);
    for (uint32_t i = 0; i < one.size(); ++i) {
        DefId id;
        id.value = i;
        CHECK(one.get(id).defName() == two.get(id).defName());
    }
    CHECK(one.idsOfKind("Alpha").size() == 3);
    CHECK(one.kinds() == std::vector<std::string>{"Alpha", "Zed"});
    CHECK_FALSE(one.idOf("Alpha", "Nope").valid());
    CHECK_FALSE(one.idOf("Nope", "A1").valid());
}

TEST_CASE("defs: same rules as check_defs.py") {
    DefDatabase db;
    std::vector<std::string> errors;

    CHECK_FALSE(defsTestBuild({{"K", "bad.json", "[{"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "bad.json: invalid JSON"));
    CHECK(db.size() == 0);

    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "obj.json", "{}"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "must be a JSON array"));

    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "n.json", R"([{"label":"x"},{"defName":""},3])"}}, "", db, errors));
    CHECK(errors.size() == 3);

    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "a.json", R"([{"defName":"X"}])"}, {"K", "b.json", R"([{"defName":"X"}])"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "duplicate K defName 'X' (also in a.json)"));

    // Same name in different kinds is fine.
    errors.clear();
    CHECK(defsTestBuild({{"K", "a.json", R"([{"defName":"X"}])"}, {"L", "a.json", R"([{"defName":"X"}])"}}, "", db, errors));

    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "p.json", R"([{"defName":"X","parent":"Missing"}])"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "unknown parent 'Missing'"));

    // A parent in another kind doesn't count.
    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "p.json", R"([{"defName":"X","parent":"Y"}])"}, {"L", "p.json", R"([{"defName":"Y"}])"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "unknown parent 'Y'"));

    errors.clear();
    CHECK_FALSE(defsTestBuild({{"K", "c.json", R"([{"defName":"A","parent":"B"},{"defName":"B","parent":"A"},{"defName":"S","parent":"S"}])"}}, "", db, errors));
    CHECK(defsTestHasError(errors, "parent cycle"));
    CHECK(errors.size() == 3);
}

TEST_CASE("defs: tuning multipliers") {
    const std::vector<DefSource> src = {{"Things", "a.json", R"([
        {"defName":"Base","abstract":true,"stats":{"hp":100}},
        {"defName":"Wall","parent":"Base","stats":{"speed":2.5,"name":"x"}},
        {"defName":"Door","parent":"Base"}
    ])"}, {"Pawns", "p.json", R"([{"defName":"Colonist","stats":{"hp":100}}])"}};
    const std::string tuning = R"({
        "defs": {"Things": {"stats.hp": 1.5, "stats.speed": 2, "stats.name": 3, "stats.missing": 9}},
        "values": {"work.speed": 0.5}
    })";
    DefDatabase db;
    std::vector<std::string> errors;
    if (!(defsTestBuild(src, tuning, db, errors))) { CHECK(false); return; }
    CHECK(db.find("Things", "Wall")->integer("stats.hp") == 150);
    CHECK(db.find("Things", "Door")->integer("stats.hp") == 150);
    CHECK(db.find("Things", "Wall")->number("stats.speed") == doctest::Approx(5.0));
    CHECK(db.find("Things", "Wall")->text("stats.name") == "x");
    CHECK_FALSE(db.find("Things", "Wall")->has("stats.missing"));
    CHECK(db.find("Pawns", "Colonist")->integer("stats.hp") == 100);  // other kinds untouched
    CHECK(db.tuning("work.speed") == doctest::Approx(0.5));
    CHECK(db.tuning("unset") == doctest::Approx(1.0));

    errors.clear();
    CHECK_FALSE(defsTestBuild(src, "[1]", db, errors));
    CHECK_FALSE(defsTestBuild(src, R"({"defs":{"Things":{"stats.hp":"big"}}})", db, errors));
    CHECK(errors.size() == 2);
}

TEST_CASE("defs: the real data folder loads") {
    DefDatabase db;
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    if (!(ok)) { CHECK(false); return; }
    CHECK(db.size() >= 4);
    CHECK(db.find("Terrain", "Soil") != nullptr);
    CHECK(db.find("Terrain", "TerrainBase") == nullptr);

    errors.clear();
    DefDatabase missing;
    CHECK_FALSE(DefDatabase::loadDirectory(COLONY_DATA_DIR "/nope", missing, errors));
    CHECK_FALSE(errors.empty());
}

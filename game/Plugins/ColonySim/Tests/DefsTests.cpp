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

    // The hand-written starting data (docs/design/starting-data.md): a few numbers per kind that
    // C1, C2, C8 and F1 read, checked after inheritance.
    const Def* human = db.find("Things", "Human");
    if (human == nullptr) { CHECK(false); return; }
    CHECK(human->text("category") == "Pawn");
    CHECK(human->number("stats.moveSpeed") == doctest::Approx(4.6 / 30.0).epsilon(0.001));
    CHECK(human->text("race.body") == "Human");
    const Def* oak = db.find("Things", "Plant_TreeOak");
    if (oak == nullptr) { CHECK(false); return; }
    CHECK(oak->text("plant.harvestedThingDef") == "WoodLog");     // from TreeBase
    CHECK(oak->number("plant.harvestWork") == doctest::Approx(1400 * 1.44));  // ticks to game seconds
    CHECK(oak->number("fillPercent") == doctest::Approx(0.25));
    // D4: crops, climates and weather.
    const char* const crops[] = {"Plant_Rice", "Plant_Potato", "Plant_Corn", "Plant_Strawberry",
                                 "Plant_Haygrass", "Plant_Cotton", "Plant_Healroot", "Plant_Devilstrand"};
    for (const char* name : crops) {
        const Def* crop = db.find("Things", name);
        if (crop == nullptr) { CHECK(false); continue; }
        CHECK(crop->flag("plant.isCrop"));
        CHECK(crop->number("plant.growDays") > 0.0);
        CHECK(crop->number("plant.growOptimalMinTemperature") == doctest::Approx(6.0));   // from CropBase
        CHECK(db.find("Things", crop->text("plant.harvestedThingDef")) != nullptr);
    }
    CHECK(db.find("Things", "Plant_Potato")->number("plant.growDays") == doctest::Approx(7.6));
    for (const char* name : {"TemperateForest", "BorealForest", "AridShrubland"}) {
        const Def* biome = db.find("Biomes", name);
        if (biome == nullptr) { CHECK(false); continue; }
        CHECK(biome->number("climate.dailySwing") > 0.0);
        CHECK(biome->number("climate.seasonMeanTemperature.winter") < biome->number("climate.seasonMeanTemperature.summer"));
        CHECK(biome->number("weather.Clear") > 0.0);
    }
    const Def* snow = db.find("Weathers", "SnowHard");
    if (snow == nullptr) { CHECK(false); return; }
    CHECK(snow->number("skyGlow") < 1.0);
    CHECK(snow->number("accuracyFactor") < 1.0);
    CHECK(snow->number("moveSpeedFactor") == doctest::Approx(0.8));  // wiki: heavy snow 80%
    CHECK(snow->number("snowRate") == doctest::Approx(1.2));
    CHECK(snow->numberAt("temperatureRange", 1) == doctest::Approx(-0.5));
    for (const char* name : {"TemperateForest", "BorealForest", "AridShrubland"}) {
        // Wiki: the three biomes share one weather table (snow is held back by the temperature).
        CHECK(db.find("Biomes", name)->number("weather.Clear") == doctest::Approx(18));
        CHECK(db.find("Biomes", name)->number("weather.SnowGentle") == doctest::Approx(4));
    }
    CHECK(db.find("Rules", "ClimateRules") != nullptr);
    CHECK(db.find("Weathers", "WeatherBase") == nullptr);
    const Def* soil = db.find("Terrain", "Soil");
    CHECK(soil->number("fertility") == doctest::Approx(1.0));
    CHECK(soil->number("pathCost") == doctest::Approx(2.0));       // from SoilBase
    CHECK(db.find("Bodies", "Human") != nullptr);
    CHECK(db.find("BodyParts", "Heart")->number("bleedRate") == doctest::Approx(5.0));
    CHECK(db.find("BodyParts", "Finger")->integer("hitPoints") == 8);
    CHECK(db.find("Hediffs", "Gunshot")->number("injuryProps.painPerSeverity") == doctest::Approx(0.0125));
    CHECK(db.find("Capacities", "Consciousness")->number("downedBelow") == doctest::Approx(0.3));
    CHECK(db.find("Skills", "Shooting") != nullptr);
    CHECK(db.idsOfKind("Skills").size() == 12);
    CHECK(db.find("Traits", "Industriousness")->count("degreeDatas") == 4);
    CHECK(db.find("Backstories", "Adult_Miner")->number("skillGains.Mining") == doctest::Approx(7));
    CHECK(db.find("WorkTypes", "Hauling")->flag("inPrototype"));
    CHECK(db.find("Names", "LastNames")->count("names") >= 50);
    CHECK(db.find("Rules", "SkillRules")->count("xpToNextLevel") == 21);
    CHECK(db.find("Scenarios", "Crashlanded")->integer("colonists") == 5);
    // D6: clothing, textiles, tailor benches, quality.
    const Def* duster = db.find("Things", "Apparel_Duster");
    CHECK(duster->number("stats.workToMake") == doctest::Approx(10000 * 1.44));
    CHECK(duster->number("apparel.stuffEffectMultiplierArmor") == doctest::Approx(0.3));  // x devilstrand 1.4 = 42%
    CHECK(duster->integer("costStuffCount") == 80);
    CHECK(db.find("Things", "Apparel_Parka")->number("apparel.stuffEffectMultiplierInsulationCold") == doctest::Approx(2.0));
    CHECK(db.find("Things", "DevilstrandCloth")->number("stuffProps.statFactors.armorRatingSharp") == doctest::Approx(1.4));
    CHECK(db.find("Things", "Hyperweave")->number("stuffProps.statOffsets.insulationCold") == doctest::Approx(26));
    CHECK(db.find("Things", "TailorBenchHand")->number("stats.workTableWorkSpeedFactor") == doctest::Approx(0.5));
    CHECK(db.find("Things", "TailorBenchElectric")->integer("costs.ComponentIndustrial") == 2);
    CHECK(db.find("Recipes", "Make_Apparel_Parka")->number("workAmount") == doctest::Approx(8000 * 1.44));
    CHECK(db.find("Rules", "QualityRules")->count("levels") == 7);
    CHECK(db.find("Rules", "QualityRules")->number("levels.6.armorFactor") == doctest::Approx(1.8));

    // Animals (D5, docs/design/animals.md): the 15 races, their bodies, and what they leave behind.
    const char* const animals[] = {"Hare", "Squirrel", "Deer", "WildBoar", "Muffalo", "Alpaca", "Cow", "Chicken",
                                   "Husky", "Cat", "TimberWolf", "GrizzlyBear", "Cougar", "Boomalope", "Thrumbo"};
    for (const char* name : animals) {
        const Def* animal = db.find("Things", name);
        if (animal == nullptr) { CHECK(false); MESSAGE(name); continue; }
        CHECK(animal->text("category") == "Pawn");
        CHECK(animal->text("race.intelligence") == "Animal");       // from AnimalBase
        CHECK(db.find("Bodies", animal->text("race.body")) != nullptr);
        CHECK(db.find("Things", animal->text("race.meatDef")) != nullptr);
        CHECK(animal->number("race.meatAmount") > 0);
        CHECK(animal->count("race.lifeStages") == 3);
        CHECK(animal->number("stats.moveSpeed") > 0);
    }
    CHECK(db.find("Things", "Chicken")->text("race.body") == "BirdAnimal");
    CHECK(db.find("Things", "Muffalo")->text("race.body") == "QuadrupedAnimal");
    CHECK(db.find("Things", "Cow")->number("race.milk.amount") > 0);
    CHECK(db.find("Things", "Alpaca")->text("race.wool.thing") == "WoolAlpaca");
    CHECK(db.find("Things", "EggChickenFertilized")->text("hatches.into") == "Chicken");
    CHECK(db.find("Things", "Meat_Deer")->number("stats.nutrition") == doctest::Approx(0.05));  // from MeatBase

    errors.clear();
    DefDatabase missing;
    CHECK_FALSE(DefDatabase::loadDirectory(COLONY_DATA_DIR "/nope", missing, errors));
    CHECK_FALSE(errors.empty());
}

TEST_CASE("defs: D8 production data (work tables, blocks, melee weapons)") {
    DefDatabase db;
    std::vector<std::string> errors;
    if (!DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors)) { CHECK(false); return; }
    for (const char* bench : {"CraftingSpot", "ButcherTable", "StonecutterTable", "FueledSmithy", "ElectricSmithy",
                              "MachiningTable", "ElectricStove"})
        CHECK(db.find("Things", bench) != nullptr);
    const Def* spear = db.find("Things", "Melee_Spear");
    if (spear == nullptr) { CHECK(false); return; }
    CHECK(spear->text("equipmentType") == "Primary");  // from MeleeWeaponBase
    CHECK(spear->number("stats.marketValue") == doctest::Approx(186));
    CHECK(spear->number("tools.0.cooldownTime") == doctest::Approx(2.6 * 30));  // RimWorld seconds x 30
    for (const char* w : {"Knife", "Club", "Gladius", "Spear", "Mace", "Longsword", "BreachAxe"}) {
        CHECK(db.find("Things", std::string("Melee_") + w) != nullptr);
        CHECK(db.find("Recipes", std::string("Make") + w) != nullptr);
    }
    const Def* blocks = db.find("Recipes", "MakeBlocksGranite");
    if (blocks == nullptr) { CHECK(false); return; }
    CHECK(blocks->number("products.BlocksGranite") == doctest::Approx(20));
    CHECK(db.find("Things", "BlocksMarble") != nullptr);
}

TEST_CASE("defs: medicine and disease data (D7)") {
    DefDatabase db;
    std::vector<std::string> errors;
    if (!DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors)) { CHECK(false); return; }

    // Medicine items: potency and the best tend quality each allows.
    const Def* herbal = db.find("Things", "MedicineHerbal");
    const Def* medicine = db.find("Things", "MedicineIndustrial");
    const Def* lumen = db.find("Things", "MedicineLumen");
    if (herbal == nullptr || medicine == nullptr || lumen == nullptr) { CHECK(false); return; }
    CHECK(herbal->number("stats.medicalPotency") == doctest::Approx(0.6));
    CHECK(medicine->number("stats.medicalPotency") == doctest::Approx(1.0));
    CHECK(lumen->number("stats.medicalPotency") == doctest::Approx(1.6));
    CHECK(herbal->number("stats.medicalQualityMax") == doctest::Approx(0.7));
    CHECK(lumen->number("stats.medicalQualityMax") == doctest::Approx(1.3));
    CHECK(herbal->count("thingCategories") == 1);

    // Diseases inherit from DiseaseBase and every stage names real capacities (check_defs.py also checks that).
    for (const char* name : {"Infection", "Flu", "Plague", "Malaria", "SleepingSickness", "GutWorms", "MuscleParasites", "FoodPoisoning"}) {
        const Def* d = db.find("Hediffs", name);
        if (d == nullptr) { CHECK_MESSAGE(false, name); continue; }
        CHECK_MESSAGE(d->text("hediffClass") == "Disease", name);
        CHECK_MESSAGE(d->count("stages") >= 1, name);
    }
    CHECK(db.find("Hediffs", "Plague")->number("lethalSeverity") == doctest::Approx(1.0));
    CHECK(db.find("Hediffs", "GutWorms")->number("lethalSeverity") == doctest::Approx(0.0));
    for (const char* name : {"Hypothermia", "Heatstroke", "Frostbite", "ToxicBuildup", "Scar", "OldWound"}) {
        CHECK_MESSAGE(db.find("Hediffs", name) != nullptr, name);
    }

    // Every artificial part is a hediff, an item and an install surgery that agree with each other.
    int artificial = 0;
    for (const DefId id : db.idsOfKind("Hediffs")) {
        const Def& h = db.get(id);
        if (h.text("hediffClass") != "AddedPart") continue;
        ++artificial;
        CHECK_MESSAGE(db.find("Things", h.text("addedPart.item").c_str()) != nullptr, h.defName());
        CHECK_MESSAGE(db.find("BodyParts", h.text("addedPart.part").c_str()) != nullptr, h.defName());
        const Def* surgery = db.find("Recipes", ("Install" + h.defName()).c_str());
        if (surgery == nullptr) { CHECK_MESSAGE(false, h.defName()); continue; }
        CHECK_MESSAGE(surgery->text("surgery.addsHediff") == h.defName(), h.defName());
        CHECK_MESSAGE(surgery->text("surgery.part") == h.text("addedPart.part"), h.defName());
        CHECK_MESSAGE(surgery->text("workSkill") == "Medicine", h.defName());   // from SurgeryBase
    }
    CHECK(artificial == 9);
    CHECK(db.find("Hediffs", "PegLeg")->number("addedPart.efficiency") < 1.0);
    CHECK(db.find("Hediffs", "BionicEye")->number("addedPart.efficiency") > 1.0);
    CHECK(db.find("Recipes", "InstallBionicArm")->integer("minSkill") == 8);
    CHECK(db.find("Recipes", "InstallPegLeg")->integer("minSkill") == 0);

    // The hospital bed is a bed that heals like HealthRules says.
    const Def* hospital = db.find("Things", "HospitalBed");
    if (hospital == nullptr) { CHECK(false); return; }
    CHECK(hospital->flag("building.bed"));
    CHECK(hospital->flag("building.medical"));
    CHECK(hospital->number("stats.medicalTendQualityOffset") > 0.0);
    const Def* healthRules = db.find("Rules", "HealthRules");
    CHECK(healthRules->number("restHealBonusPerDay.HospitalBed") > healthRules->number("restHealBonusPerDay.Bed"));
    CHECK(db.find("Rules", "MedicineRules")->count("tendQualityBySkill") == 21);
}

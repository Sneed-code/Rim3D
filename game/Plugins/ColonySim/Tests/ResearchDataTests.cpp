#include "ColonySim/Defs.h"

#include <doctest.h>

#include <set>
#include <string>
#include <vector>

using namespace colony;

// D12: research projects (docs/design/research-data.md).
TEST_CASE("research data: projects load with costs, tech levels and prerequisites") {
    DefDatabase db;
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    if (!ok) { CHECK(false); return; }

    for (const char* name : {"Neolithic", "Medieval", "Industrial", "Spacer"}) {
        CHECK_MESSAGE(db.find("TechLevels", name) != nullptr, name);
    }
    CHECK(db.find("TechLevels", "Medieval")->number("costFactorForNeolithicStart") == doctest::Approx(1.5));

    for (const char* name : {"ComplexClothing", "ComplexFurniture", "Smithing", "Stonecutting", "Electricity",
                             "Battery", "SolarPanel", "AirConditioning", "Machining", "Prosthetics",
                             "Microelectronics", "BionicReplacements", "PassiveCooler", "Devilstrand"}) {
        const Def* d = db.find("Research", name);
        CHECK_MESSAGE(d != nullptr, name);
        if (d == nullptr) continue;
        CHECK_MESSAGE(d->number("baseCost") > 0, name);
        CHECK_MESSAGE(db.find("TechLevels", d->text("techLevel").c_str()) != nullptr, name);
    }
    CHECK(db.find("Research", "Electricity")->number("baseCost") == doctest::Approx(1600));
    CHECK(db.find("Research", "Smithing")->number("baseCost") == doctest::Approx(700));
    CHECK(db.find("Research", "Machining")->count("prerequisites") == 2);
    CHECK(db.find("Research", "Microelectronics")->text("unlocks.Things.0") == "HiTechResearchBench");

    // Every project's prerequisites exist and the tree has no cycle (a project can be reached from nothing).
    std::set<std::string> done;
    const std::vector<const char*> all = {"PassiveCooler", "Devilstrand", "ComplexClothing", "ComplexFurniture", "CarpetMaking",
        "Smithing", "Stonecutting", "LongBlades", "DrugProduction", "Electricity", "Battery", "SolarPanel",
        "AirConditioning", "SterileMaterials", "Machining", "Prosthetics", "Microelectronics", "HospitalBed",
        "MedicineProduction", "MultiAnalyzer", "Fabrication", "BionicReplacements"};
    for (size_t round = 0; round < 40; ++round) {
        for (const char* name : all) {
            const Def* d = db.find("Research", name);
            if (d == nullptr) { CHECK_MESSAGE(false, name); continue; }
            bool ready = true;
            for (const char* key : {"prerequisites", "hiddenPrerequisites"}) {
                for (size_t i = 0; i < d->count(key); ++i) {
                    ready = ready && done.count(d->textAt(key, i)) > 0;
                }
            }
            if (ready) done.insert(name);
        }
    }
    CHECK(done.size() == all.size());

    // Gated content carries its project, also through a parent (hand tailor bench via TailorBenchBase).
    CHECK(db.find("Things", "TailorBenchHand")->text("researchPrerequisite") == "ComplexClothing");
    CHECK(db.find("Things", "SolarGenerator")->text("researchPrerequisite") == "SolarPanel");
    CHECK(db.find("Things", "ElectricSmithy")->count("researchPrerequisite") == 2);
    CHECK(db.find("Terrain", "Carpet")->text("researchPrerequisite") == "CarpetMaking");
    CHECK(db.find("Recipes", "InstallBionicEye")->text("researchPrerequisite") == "BionicReplacements");

    const Def* simple = db.find("Things", "SimpleResearchBench");
    const Def* hiTech = db.find("Things", "HiTechResearchBench");
    if (simple == nullptr || hiTech == nullptr) { CHECK(false); return; }
    CHECK(simple->number("researchBench.speedFactor") == doctest::Approx(0.75));
    CHECK(hiTech->number("researchBench.speedFactor") == doctest::Approx(1.0));
    CHECK(hiTech->number("researchBench.tier") > simple->number("researchBench.tier"));
}

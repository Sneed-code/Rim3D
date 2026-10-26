#include "ColonySim/Defs.h"

#include <doctest.h>

#include <string>
#include <vector>

using namespace colony;

// D10: narrators, difficulties, incidents, factions, traders (docs/design/narrator-data.md).
TEST_CASE("narrator data: narrators, difficulties and rules load") {
    DefDatabase db;
    std::vector<std::string> errors;
    const bool ok = DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors);
    for (const std::string& e : errors) MESSAGE(e);
    if (!ok) { CHECK(false); return; }

    for (const char* name : {"Orrin", "Maren", "Jinx"}) CHECK_MESSAGE(db.find("Narrators", name) != nullptr, name);
    CHECK(db.find("Narrators", "Orrin")->number("threatCycle.offDays") > 0);
    CHECK(db.find("Narrators", "Maren")->number("threatCycle.offDays") > db.find("Narrators", "Orrin")->number("threatCycle.offDays"));
    CHECK(db.find("Narrators", "Jinx")->count("threatCycle") == 0);
    CHECK(db.find("Narrators", "Jinx")->number("mtbDays.Misc") == doctest::Approx(3.56));

    const char* levels[] = {"Peaceful", "CommunityBuilder", "AdventureStory", "StriveToSurvive", "BloodAndDust", "LosingIsFun"};
    double lastScale = 0;
    for (const char* name : levels) {
        const Def* d = db.find("Difficulties", name);
        CHECK_MESSAGE(d != nullptr, name);
        if (d == nullptr) continue;
        CHECK_MESSAGE(d->number("threatScale") > lastScale, name);  // each level is harder than the last
        lastScale = d->number("threatScale");
    }
    CHECK(db.find("Difficulties", "StriveToSurvive")->number("threatScale") == doctest::Approx(1.0));
    CHECK(db.find("Difficulties", "LosingIsFun")->number("threatScale") == doctest::Approx(2.2));
    CHECK(db.find("Difficulties", "LosingIsFun")->number("tradePriceDisadvantage") == doctest::Approx(0.2));
    // The raid rules' threat scale is the default difficulty's.
    CHECK(db.find("Rules", "RaidRules")->number("threatScale") ==
          doctest::Approx(db.find("Difficulties", db.find("Rules", "NarratorRules")->text("defaultDifficulty").c_str())->number("threatScale")));

    CHECK(db.find("Rules", "NarratorRules")->count("populationIntent") == 6);
    CHECK(db.find("Rules", "TradeRules")->number("colonyBuysAt") > db.find("Rules", "TradeRules")->number("colonySellsAt"));
    CHECK(db.find("Rules", "FactionRules")->number("hostileAtOrBelow") < 0);
}

TEST_CASE("narrator data: incidents have a category, chance and earliest day") {
    DefDatabase db;
    std::vector<std::string> errors;
    if (!DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors)) { CHECK(false); return; }

    const Def* raid = db.find("Incidents", "RaidEnemy");
    if (raid == nullptr) { CHECK(false); return; }
    CHECK(raid->text("category") == "ThreatBig");
    CHECK(raid->number("baseChance") > 0);
    for (const char* name : {"ManhunterPack", "Infestation", "Disease_Flu", "Disease_Plague", "ColdSnap", "HeatWave", "Eclipse",
                             "Aurora", "WandererJoin", "ResourcePodCrash", "RefugeePodCrash", "HerdMigration", "AnimalSelfTame",
                             "TraderCaravanArrival", "VisitorGroup", "OrbitalTraderArrival", "PrisonBreak", "ShortCircuit",
                             "SolarFlare", "ToxicFallout", "Blight"}) {
        const Def* d = db.find("Incidents", name);
        CHECK_MESSAGE(d != nullptr, name);
        if (d == nullptr) continue;
        CHECK_MESSAGE(!d->text("category").empty(), name);
        CHECK_MESSAGE(d->number("baseChance") > 0, name);
        CHECK_MESSAGE(d->number("earliestDay", -1) >= 0, name);
    }
    // Trouble waits until the colony has had time to settle.
    CHECK(db.find("Incidents", "Infestation")->number("earliestDay") > raid->number("earliestDay"));
    CHECK(db.find("Incidents", "OrbitalTraderArrival")->number("stayHours") == doctest::Approx(16));
}

TEST_CASE("narrator data: factions, pawn kinds and trader kinds") {
    DefDatabase db;
    std::vector<std::string> errors;
    if (!DefDatabase::loadDirectory(COLONY_DATA_DIR, db, errors)) { CHECK(false); return; }

    for (const char* name : {"Pirate", "FrayCompact", "FreeHold", "ClanGentle", "ClanFierce", "ClanSavage", "Concord", "OldCrews", "Remnant", "Chitinid"}) {
        CHECK_MESSAGE(db.find("Factions", name) != nullptr, name);
    }
    CHECK(db.find("Factions", "Pirate")->flag("permanentEnemy"));
    CHECK(db.find("Factions", "ClanSavage")->flag("permanentEnemy"));
    CHECK_FALSE(db.find("Factions", "FrayCompact")->flag("permanentEnemy"));
    CHECK(db.find("Factions", "FrayCompact")->number("startingGoodwill.0") <= db.find("Factions", "FrayCompact")->number("startingGoodwill.1"));
    CHECK(db.find("Factions", "FrayCompact")->count("traders.caravan") > 0);

    // Every faction that visits has visitor kinds; every one that trades has trader kinds and guards.
    for (const char* name : {"FrayCompact", "FreeHold", "ClanGentle", "ClanFierce"}) {
        const Def* f = db.find("Factions", name);
        CHECK_MESSAGE(f->flag("canVisit"), name);
        CHECK_MESSAGE(f->count("visitorKinds") > 0, name);
        CHECK_MESSAGE(f->count("guardKinds") > 0, name);
        CHECK_MESSAGE(f->count("traderKinds") > 0, name);
        CHECK_MESSAGE(f->count("traderGuardKinds") > 0, name);
    }
    CHECK_FALSE(db.find("Factions", "OldCrews")->flag("canRaid"));

    CHECK(db.find("PawnKinds", "FrayTrader")->text("role") == "trader");
    CHECK(db.find("PawnKinds", "PirateGunner")->text("role") == "raider");
    CHECK(db.find("PawnKinds", "ClanWarrior")->number("combatPower") > 0);

    const Def* bulk = db.find("TraderKinds", "BulkGoods");
    if (bulk == nullptr) { CHECK(false); return; }
    CHECK(bulk->number("silver.0") == doctest::Approx(750));
    CHECK(bulk->number("silver.1") == doctest::Approx(1200));
    CHECK(bulk->count("stock") > 0);
    for (const char* name : {"CombatSupplier", "ExoticGoods", "ShamanMerchant", "Slaver", "WarMerchant"}) {
        CHECK_MESSAGE(db.find("TraderKinds", name) != nullptr, name);
    }
}

#include "ColonySim/Apparel.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Health.h"
#include "ColonySim/Jobs.h"
#include "ColonySim/Mood.h"
#include "ColonySim/Replay.h"
#include "ColonySim/Save.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Temperature.h"
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace colony;

// Exceptions are off, so doctest's REQUIRE can't be used: check, and leave the test on failure.
#define APPAREL_REQUIRE(...)   \
    do {                       \
        CHECK(__VA_ARGS__);    \
        if (!(__VA_ARGS__)) {  \
            return;            \
        }                      \
    } while (0)

namespace {

// Tests for docs/design/apparel.md, on the game's real data.
std::shared_ptr<const DefDatabase> apparelTestDefs() {
    static const std::shared_ptr<const DefDatabase> defs = [] {
        auto db = std::make_shared<DefDatabase>();
        std::vector<std::string> errors;
        DefDatabase::loadDirectory(COLONY_DATA_DIR, *db, errors);
        for (const std::string& e : errors) {
            MESSAGE(e);
        }
        return db;
    }();
    return defs;
}

std::unique_ptr<Sim> apparelTestSim(uint64_t seed = 21) {
    SimSettings settings;
    settings.seed = seed;
    settings.mapSize = 96;
    settings.flatMap = true;
    return std::make_unique<Sim>(settings, apparelTestDefs());
}

DefId apparelDef(const Sim& sim, const char* name) {
    return sim.defs().idOf("Things", name);
}

// A colonist: a pawn that works (and so thinks and has a mood), with a Colonist record.
Entity apparelColonist(Sim& sim, int x, int y, bool female = false, const char* trait = nullptr) {
    const Entity pawn = sim.things().spawn("Human", CellPos{x, y, 0});
    sim.jobs().addWorker(pawn);
    Colonist colonist;
    colonist.female = female;
    if (trait != nullptr) {
        colonist.traits.push_back(TraitRecord{trait, 0});
    }
    sim.things().registry().emplace<Colonist>(pawn, std::move(colonist));
    return pawn;
}

// Makes a piece (cloth unless the def isn't made of a stuff) and puts it on the pawn.
Entity apparelWear(Sim& sim, Entity pawn, const char* def, int quality = -1, const char* stuff = "Cloth") {
    const Position& at = sim.things().registry().get<Position>(pawn);
    const bool stuffed = sim.defs().get(apparelDef(sim, def)).flag("stuffRequired");
    const Entity item = sim.apparel().make(apparelDef(sim, def), stuffed ? apparelDef(sim, stuff) : DefId{}, quality, cellOf(at));
    std::string why;
    if (item == kNoEntity || !sim.apparel().putOn(pawn, item, why)) {
        FAIL_CHECK("could not put on ", def, ": ", why);
    }
    return item;
}

Entity apparelLying(Sim& sim, int x, int y, const char* def, int quality = -1, const char* stuff = "Cloth") {
    const bool stuffed = sim.defs().get(apparelDef(sim, def)).flag("stuffRequired");
    return sim.apparel().make(apparelDef(sim, def), stuffed ? apparelDef(sim, stuff) : DefId{}, quality, CellPos{x, y, 0});
}

uint32_t apparelOutfitId(const Sim& sim, const char* name) {
    for (const OutfitPolicy& o : sim.apparel().outfits()) {
        if (o.name == name) {
            return o.id;
        }
    }
    return 0;
}

const ThoughtView* apparelThought(const std::vector<ThoughtView>& thoughts, const char* def) {
    for (const ThoughtView& t : thoughts) {
        if (t.def == def) {
            return &t;
        }
    }
    return nullptr;
}

std::vector<ThoughtView> apparelThoughts(const Sim& sim, Entity pawn) {
    return sim.mood().thoughts(pawn, sim.time().seconds);
}

void apparelSetHitPoints(Sim& sim, Entity item, int hp) {
    sim.things().registry().get<HitPoints>(item).current = hp;
}

int apparelHitPoints(const Sim& sim, Entity item) {
    return sim.things().registry().get<HitPoints>(item).current;
}

float apparelDurability(const Sim& sim, Entity item) { return ApparelStats::condition(sim.things().registry(), item); }

void apparelSetDurability(Sim& sim, Entity item, float share) {
    sim.things().registry().emplace_or_replace<Durability>(item, Durability{share});
}

bool apparelWearing(const Sim& sim, Entity pawn, Entity item) {
    const std::vector<Entity>& worn = sim.apparel().worn(pawn);
    return std::find(worn.begin(), worn.end(), item) != worn.end();
}

// A human made by the spawn command, so it has looks, and a mood.
Entity apparelSpawnHuman(Sim& sim, int x, int y) {
    sim.submit(SpawnThingCommand{"Human", CellPos{x, y, 0}, 1});
    sim.step();
    for (const Event& e : sim.events()) {
        if (e.type == EventType::ThingSpawned && sim.things().registry().get<Thing>(e.thing).category == ThingCategory::Pawn) {
            return e.thing;
        }
    }
    return kNoEntity;
}

}  // namespace

TEST_CASE("Apparel: mitigation, weight, insulation and hit points come from the stuff and the quality") {
    auto sim = apparelTestSim();
    const ApparelStats& stats = sim->apparel().stats();
    const entt::registry& reg = sim->things().registry();

    // Every regular garment: +10 cold, +2 heat (sneed), times the quality's insulation factor.
    const Entity parka = apparelLying(*sim, 10, 10, "Apparel_Parka");
    CHECK(stats.insulationCold(reg, parka) == doctest::Approx(10.0f));
    CHECK(stats.insulationHeat(reg, parka) == doctest::Approx(2.0f));
    const Entity good = apparelLying(*sim, 11, 10, "Apparel_Parka", 3);
    CHECK(stats.insulationCold(reg, good) == doctest::Approx(10.0f * 1.1f));
    CHECK(stats.qualityOf(reg, parka) == 2);
    CHECK(stats.qualityOf(reg, good) == 3);

    // A devilstrand duster: devilstrand's 6 / 6 / 12, 200 hit points x 1.3, 0.6 kg. Masterwork: 75% more.
    const Entity duster = apparelLying(*sim, 12, 10, "Apparel_Duster", -1, "DevilstrandCloth");
    CHECK(stats.mitigation(reg, duster).pct[1] == doctest::Approx(6.0f));
    CHECK(stats.mitigation(reg, duster).pct[2] == doctest::Approx(12.0f));
    CHECK(stats.weightKg(reg, duster) == doctest::Approx(0.6f));
    CHECK(sim->things().registry().get<HitPoints>(duster).max == 260);
    CHECK(apparelHitPoints(*sim, duster) == 260);
    const Entity master = apparelLying(*sim, 13, 10, "Apparel_Duster", 5, "DevilstrandCloth");
    CHECK(stats.mitigation(reg, master).pct[2] == doctest::Approx(12.0f * 1.75f));

    // A cloth T-shirt: cotton's 4 / 4 / 2, the same as any other cloth shirt; a flak vest keeps its own.
    const Entity shirt = apparelLying(*sim, 14, 10, "Apparel_TShirt");
    CHECK(stats.mitigation(reg, shirt).pct[0] == doctest::Approx(4.0f));
    CHECK(stats.mitigation(reg, shirt).pct[2] == doctest::Approx(2.0f));
    CHECK(stats.weightKg(reg, shirt) == doctest::Approx(1.0f));
    CHECK(stats.gearKind(apparelDef(*sim, "Apparel_TShirt")) == GearKind::Clothing);
    const Entity vest = apparelLying(*sim, 15, 10, "Apparel_FlakVest");
    CHECK(stats.mitigation(reg, vest).pct[1] == doctest::Approx(30.0f));
    CHECK(stats.mitigation(reg, vest).pct[0] == doctest::Approx(15.0f));
    CHECK(stats.weightKg(reg, vest) == doctest::Approx(4.0f));
    CHECK(stats.gearKind(apparelDef(*sim, "Apparel_FlakVest")) == GearKind::Armour);
    CHECK(apparelHitPoints(*sim, vest) == 200);

    // A hat gets half its material's %s and weight; legendary doubles; awful halves.
    const Entity hat = apparelLying(*sim, 16, 11, "Apparel_CowboyHat", -1, "Synthread");
    CHECK(stats.mitigation(reg, hat).pct[0] == doctest::Approx(5.0f));
    CHECK(stats.mitigation(reg, hat).pct[5] == doctest::Approx(5.0f));  // synthread's toxic extra, halved
    CHECK(stats.weightKg(reg, hat) == doctest::Approx(0.125f));
    const Entity legendary = apparelLying(*sim, 17, 11, "Apparel_Pants", 6);
    CHECK(stats.mitigation(reg, legendary).pct[0] == doctest::Approx(8.0f));
    const Entity awful = apparelLying(*sim, 18, 11, "Apparel_Pants", 0);
    CHECK(stats.mitigation(reg, awful).pct[0] == doctest::Approx(2.0f));

    // Wolf leather weighs nothing; plant fibre takes more fire, and quality doesn't make that worse.
    const Entity wolf = apparelLying(*sim, 19, 11, "Apparel_Pants", -1, "Leather_Wolf");
    CHECK(stats.weightKg(reg, wolf) == doctest::Approx(0.0f));
    const Entity tribal = apparelLying(*sim, 20, 11, "Apparel_Tribalwear", 6, "PlantFibre");
    CHECK(stats.mitigation(reg, tribal).pct[0] == doctest::Approx(4.0f));
    CHECK(stats.mitigation(reg, tribal).pct[3] == doctest::Approx(-8.0f));

    // Plate armour takes its metal: plasteel 1.2 / 1.25 / 1.1 of steel's 20 / 30 / 10; wood loses its elemental
    // and takes more fire.
    const Entity plate = apparelLying(*sim, 21, 11, "Apparel_PlateArmor", -1, "Plasteel");
    CHECK(stats.mitigation(reg, plate).pct[0] == doctest::Approx(24.0f));
    CHECK(stats.mitigation(reg, plate).pct[1] == doctest::Approx(37.5f));
    CHECK(stats.mitigation(reg, plate).pct[2] == doctest::Approx(11.0f));
    CHECK(stats.weightKg(reg, plate) == doctest::Approx(15.0f));
    const Entity woodPlate = apparelLying(*sim, 22, 11, "Apparel_PlateArmor", -1, "WoodLog");
    CHECK(stats.mitigation(reg, woodPlate).pct[1] == doctest::Approx(18.0f));
    CHECK(stats.mitigation(reg, woodPlate).pct[2] == doctest::Approx(0.0f));
    CHECK(stats.mitigation(reg, woodPlate).pct[3] == doctest::Approx(-10.0f));

    // A garment with no stuff given is worth nothing.
    const Entity bare = sim->things().make(apparelDef(*sim, "Apparel_Tribalwear"));
    CHECK_FALSE(stats.mitigation(reg, bare).any());
    CHECK(stats.insulationCold(reg, bare) == 0.0f);

    // Things that aren't apparel are refused.
    std::string why;
    CHECK(sim->apparel().make(apparelDef(*sim, "Steel"), DefId{}, -1, CellPos{16, 10, 0}, &why) == kNoEntity);
    CHECK(sim->apparel().make(apparelDef(*sim, "Apparel_Parka"), apparelDef(*sim, "Cloth"), 9, CellPos{16, 10, 0}, &why) == kNoEntity);
}

TEST_CASE("Apparel: one shirt, pants and a hat of clothing, and armour over them, add up their insulation") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity tshirt = apparelWear(*sim, pawn, "Apparel_TShirt");
    apparelWear(*sim, pawn, "Apparel_Pants");
    // A parka is just a shirt (sneed): it replaces the T-shirt, which drops where the pawn stands.
    apparelWear(*sim, pawn, "Apparel_Parka");
    CHECK_FALSE(apparelWearing(*sim, pawn, tshirt));
    float cold = 0.0f;
    float heat = 0.0f;
    sim->apparel().insulation(pawn, cold, heat);
    CHECK(cold == doctest::Approx(20.0f));  // +10 cold, +2 heat a garment
    CHECK(heat == doctest::Approx(4.0f));
    CHECK(sim->apparel().worn(pawn).size() == 2);

    // Armour goes over clothing: a flak vest and a helmet sit on top of the shirt and a hat.
    const Entity vest = apparelWear(*sim, pawn, "Apparel_FlakVest");
    const Entity hat = apparelWear(*sim, pawn, "Apparel_Tuque");
    const Entity helmet = apparelWear(*sim, pawn, "Apparel_SimpleHelmet");
    CHECK(apparelWearing(*sim, pawn, vest));
    CHECK(apparelWearing(*sim, pawn, hat));
    CHECK(apparelWearing(*sim, pawn, helmet));
    CHECK(sim->apparel().worn(pawn).size() == 5);
    // A flak jacket is the same chest armour as the vest, so it replaces it.
    const Entity flakJacket = apparelWear(*sim, pawn, "Apparel_FlakJacket");
    CHECK_FALSE(apparelWearing(*sim, pawn, vest));
    CHECK(apparelWearing(*sim, pawn, flakJacket));
    std::string error;
    CHECK(sim->apparel().takeOff(pawn, flakJacket, error));
    CHECK(sim->apparel().takeOff(pawn, hat, error));
    CHECK(sim->apparel().takeOff(pawn, helmet, error));

    // A jacket replaces the parka the same way.
    const Entity parka = sim->apparel().worn(pawn)[1];
    const Entity jacket = apparelWear(*sim, pawn, "Apparel_Jacket");
    CHECK(sim->apparel().worn(pawn).size() == 2);
    CHECK_FALSE(apparelWearing(*sim, pawn, parka));
    CHECK(sim->things().spawned(parka));
    CHECK(apparelWearing(*sim, pawn, jacket));
    CHECK(sim->apparel().covered(pawn, "Torso"));
    CHECK(sim->apparel().covered(pawn, "Legs"));
    CHECK_FALSE(sim->apparel().covered(pawn, "UpperHead"));
}

TEST_CASE("Apparel: hits wear a worn piece's durability, not its hit points; only basic clothes can be shot to pieces") {
    auto sim = apparelTestSim();
    const Entity target = apparelColonist(*sim, 18, 30);
    const Entity vest = apparelWear(*sim, target, "Apparel_FlakVest");
    const Entity shirt = apparelWear(*sim, target, "Apparel_TShirt");
    const Entity duster = apparelWear(*sim, target, "Apparel_CowboyHat");
    CombatSystem& combat = sim->combat();

    // 5 hit points' worth of wear on a 100 hit point shirt: 5% durability, the hit points stay.
    combat.damageApparel(target, shirt, 5);
    CHECK(apparelHitPoints(*sim, shirt) == 100);
    CHECK(apparelDurability(*sim, shirt) == doctest::Approx(0.95f));

    // A cowboy hat is not a basic piece: worn right through it stays on, tattered, at full hit points.
    const int dusterHp = apparelHitPoints(*sim, duster);
    combat.damageApparel(target, duster, 100000);
    CHECK(sim->things().exists(duster));
    CHECK(apparelWearing(*sim, target, duster));
    CHECK(apparelHitPoints(*sim, duster) == dusterHp);
    CHECK(apparelDurability(*sim, duster) == doctest::Approx(0.0f));
    CHECK(ApparelStats::tattered(sim->things().registry(), duster));
    std::string error;
    CHECK(sim->apparel().takeOff(target, duster, error));

    // A T-shirt is basic (`apparel.fragile`): past 0 durability the rest comes off its hit points.
    sim->step();
    combat.damageApparel(target, shirt, 95 + 30);
    CHECK(apparelDurability(*sim, shirt) == doctest::Approx(0.0f));
    CHECK(apparelHitPoints(*sim, shirt) == 70);
    combat.damageApparel(target, shirt, 500);
    bool destroyedEvent = false;
    for (const Event& e : sim->events()) {
        destroyedEvent = destroyedEvent || (e.type == EventType::ApparelDestroyed && e.thing == target);
    }
    CHECK(destroyedEvent);
    CHECK_FALSE(sim->things().exists(shirt));
    CHECK(sim->apparel().worn(target).size() == 1);

    // Shot at: the vest wears 1 hit point's worth for every 4 points of damage it absorbs, and is never destroyed.
    const Entity shooter = sim->things().spawn("Human", CellPos{10, 30, 0});
    combat.equip(shooter, sim->things().make(apparelDef(*sim, "Gun_Revolver")));
    Rng rng(4);
    int armorEvents = 0;
    bool vestGone = false;
    const int vestMax = sim->things().registry().get<HitPoints>(vest).max;
    for (int shot = 0; shot < 400; ++shot) {
        const float before = apparelDurability(*sim, vest);
        combat.fire(shooter, 18.5f, 30.5f, 1.15f, 0.0f, rng);
        for (int s = 0; s < 20; ++s) {
            sim->step();
            for (const Event& e : sim->events()) {
                armorEvents += e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Armor ? 1 : 0;
                vestGone = vestGone || (e.type == EventType::ApparelDestroyed && e.thing == target);
            }
            if (sim->things().registry().view<Projectile>().empty()) {
                break;
            }
        }
        CHECK(before - apparelDurability(*sim, vest) <= 8.0f / static_cast<float>(vestMax) + 1.0e-3f);  // a bullet is about 12 damage
        sim->health().refresh(target);
        sim->things().registry().replace<Health>(target, Health{});
        sim->health().refresh(target);
    }
    (void)armorEvents;
    CHECK_FALSE(vestGone);
    CHECK(sim->things().exists(vest));
    CHECK(apparelWearing(*sim, target, vest));
    CHECK(apparelHitPoints(*sim, vest) == vestMax);
    CHECK(apparelDurability(*sim, vest) < 0.9f);
}

TEST_CASE("Apparel: durability is capped by hit points, and a tattered piece gives 67% of its mitigation and 80% of its warmth") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity parka = apparelWear(*sim, pawn, "Apparel_Parka");
    const entt::registry& reg = sim->things().registry();
    const int max = reg.get<HitPoints>(parka).max;
    const float armor = sim->apparel().stats().mitigation(reg, parka).pct[1];
    const float cold = sim->apparel().stats().insulationCold(reg, parka);
    APPAREL_REQUIRE(cold > 0.0f);

    // An item at 70% of its hit points is at most 70% durable.
    apparelSetHitPoints(*sim, parka, max * 7 / 10);
    CHECK(apparelDurability(*sim, parka) == doctest::Approx(0.7f).epsilon(0.02));
    apparelSetDurability(*sim, parka, 0.5f);
    CHECK(apparelDurability(*sim, parka) == doctest::Approx(0.5f));
    CHECK(sim->apparel().stats().mitigation(reg, parka).pct[1] == doctest::Approx(armor));

    apparelSetDurability(*sim, parka, 0.0f);
    CHECK(sim->apparel().stats().mitigation(reg, parka).pct[1] == doctest::Approx(armor * 0.67f));
    CHECK(sim->apparel().stats().insulationCold(reg, parka) == doctest::Approx(cold * 0.8f));
    float c = 0.0f;
    float h = 0.0f;
    sim->apparel().insulation(pawn, c, h);
    CHECK(c == doctest::Approx(cold * 0.8f));

    // The snapshot carries it.
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    bool seen = false;
    for (const ApparelView& worn : snapshot.apparel) {
        if (worn.item == parka) {
            seen = true;
            CHECK(worn.durability == doctest::Approx(0.0f));
            CHECK(worn.hitPointShare == doctest::Approx(0.7f).epsilon(0.02));
        }
    }
    CHECK(seen);
}

TEST_CASE("Apparel: every worn piece divides the damage, wherever the hit lands; the better of hat and helmet counts") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 18, 30);
    CombatSystem& combat = sim->combat();
    const DefId cut = sim->health().damageTypeId("Cut");
    const DefId bullet = sim->health().damageTypeId("Bullet");
    const DefId burn = sim->health().damageTypeId("Burn");
    const DefId slash = sim->health().damageTypeId("Slash");
    APPAREL_REQUIRE(slash.valid());
    CHECK(combat.mitigate(pawn, cut, 10.0f) == doctest::Approx(10.0f));  // naked

    // Cotton shirt and pants, 4% each: 10 / 1.04 / 1.04.
    apparelWear(*sim, pawn, "Apparel_TShirt");
    apparelWear(*sim, pawn, "Apparel_Pants");
    CHECK(combat.mitigate(pawn, cut, 10.0f) == doctest::Approx(10.0f / 1.04f / 1.04f));
    // Fire is elemental: cotton's 2%.
    CHECK(combat.mitigate(pawn, burn, 10.0f) == doctest::Approx(10.0f / 1.02f / 1.02f));

    // A flak vest (30% sharp, 15% blunt) on top. A bullet is 75% blunt, 25% sharp; a sword's slash 75% sharp, 25% blunt.
    const Entity vest = apparelWear(*sim, pawn, "Apparel_FlakVest");
    const float clothes = 1.04f * 1.04f;
    CHECK(combat.mitigate(pawn, cut, 10.0f) == doctest::Approx(10.0f / clothes / 1.3f));
    CHECK(combat.mitigate(pawn, bullet, 10.0f) == doctest::Approx(7.5f / clothes / 1.15f + 2.5f / clothes / 1.3f));
    CHECK(combat.mitigate(pawn, slash, 10.0f) == doctest::Approx(7.5f / clothes / 1.3f + 2.5f / clothes / 1.15f));
    // Armour penetration takes its share off each %: 50% leaves the vest at 15% and cotton at 2%.
    CHECK(combat.mitigate(pawn, cut, 10.0f, 0.5f) == doctest::Approx(10.0f / 1.02f / 1.02f / 1.15f));
    // Each piece's share of what was taken off, for wear.
    std::vector<std::pair<Entity, float>> shares;
    combat.mitigate(pawn, cut, 10.0f, 0.0f, false, &shares);
    float total = 0.0f;
    float vestShare = 0.0f;
    for (const auto& [item, share] : shares) {
        total += share;
        vestShare += item == vest ? share : 0.0f;
    }
    CHECK(total == doctest::Approx(1.0f));
    CHECK(vestShare == doctest::Approx(std::log(1.3f) / (std::log(1.3f) + 2.0f * std::log(1.04f))));

    // Head: a cloth tuque (2%) and a simple helmet (12% sharp): only the helmet counts against sharp.
    apparelWear(*sim, pawn, "Apparel_Tuque");
    CHECK(combat.mitigate(pawn, cut, 10.0f) == doctest::Approx(10.0f / clothes / 1.3f / 1.02f));
    apparelWear(*sim, pawn, "Apparel_SimpleHelmet");
    CHECK(combat.mitigate(pawn, cut, 10.0f) == doctest::Approx(10.0f / clothes / 1.3f / 1.12f));

    // An explosion of fire (molotov) is half blunt, half fire.
    const float blunt = 1.04f * 1.04f * 1.15f * 1.08f;
    const float fire = 1.02f * 1.02f * 1.15f * 1.06f;
    CHECK(combat.mitigate(pawn, burn, 10.0f, 0.0f, true) == doctest::Approx(5.0f / blunt + 5.0f / fire));
}

TEST_CASE("Apparel: subtypes and weaknesses multiply; mitigation never reaches zero") {
    CHECK(mitigationDivisor(10.0f) == doctest::Approx(1.1f));
    CHECK(mitigationDivisor(500.0f) == doctest::Approx(6.0f));
    CHECK(mitigationDivisor(-6.0f) == doctest::Approx(1.0f / 1.06f));
    Mitigation wool;
    wool.pct[2] = 5.0f;   // elemental
    wool.pct[3] = -5.0f;  // fire
    wool.pct[4] = 3.0f;   // cold
    CHECK(mitigationFactor(wool, DamageClass::Elemental, DamageSub::Fire, 0.0f) == doctest::Approx(1.05f / 1.05f));
    CHECK(mitigationFactor(wool, DamageClass::Elemental, DamageSub::Cold, 0.0f) == doctest::Approx(1.05f * 1.03f));
    CHECK(mitigationFactor(wool, DamageClass::Elemental, DamageSub::None, 0.0f) == doctest::Approx(1.05f));
    CHECK(mitigationFactor(wool, DamageClass::Blunt, DamageSub::Fire, 0.0f) == doctest::Approx(1.0f));
    // Penetration leaves weaknesses alone.
    CHECK(mitigationFactor(wool, DamageClass::Elemental, DamageSub::Fire, 1.0f) == doctest::Approx(1.0f / 1.05f));

    // A mechanoid's hull: 1000% fire, and electric and acid hurt it more.
    auto sim = apparelTestSim();
    const Entity scyther = sim->things().spawn("Mech_Scyther", CellPos{30, 30, 0});
    APPAREL_REQUIRE(scyther != kNoEntity);
    CHECK(sim->combat().mitigate(scyther, sim->health().damageTypeId("Burn"), 22.0f) == doctest::Approx(22.0f / 1.1f / 11.0f));
    CHECK(sim->combat().mitigate(scyther, sim->health().damageTypeId("Cut"), 12.0f) == doctest::Approx(10.0f));
}

TEST_CASE("Apparel: worn clothes wear slowly, 0.4 hit points' worth a day, and never lose hit points by it") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    std::vector<Entity> pieces = {apparelWear(*sim, pawn, "Apparel_TShirt"), apparelWear(*sim, pawn, "Apparel_Pants"),
                                  apparelWear(*sim, pawn, "Apparel_FlakVest"), apparelWear(*sim, pawn, "Apparel_CowboyHat")};
    std::vector<int> hitPoints;
    for (const Entity p : pieces) {
        hitPoints.push_back(apparelHitPoints(*sim, p));
    }
    Rng rng(3);
    const std::vector<Event> none;
    for (int hour = 0; hour < 24 * 20; ++hour) {
        sim->apparel().step(static_cast<int64_t>(hour) * 3600, rng, none);
    }
    // 20 days x 0.4 = 8 hit points' worth each, as a share of the piece's maximum.
    for (size_t i = 0; i < pieces.size(); ++i) {
        CHECK(apparelHitPoints(*sim, pieces[i]) == hitPoints[i]);
        CHECK(apparelDurability(*sim, pieces[i]) == doctest::Approx(1.0f - 8.0f / static_cast<float>(hitPoints[i])).epsilon(0.001));
    }
    // Not at all between the hours.
    const float now = apparelDurability(*sim, pieces[0]);
    for (int i = 0; i < 50; ++i) {
        sim->apparel().step(3600 * 1000 + 1 + i, rng, none);
    }
    CHECK(apparelDurability(*sim, pieces[0]) == now);

    // A whole year (60 days) of wear leaves a 100 hit point shirt well over half: 76%.
    for (int hour = 24 * 20; hour < 24 * 60; ++hour) {
        sim->apparel().step(static_cast<int64_t>(hour) * 3600, rng, none);
    }
    CHECK(apparelDurability(*sim, pieces[0]) == doctest::Approx(0.76f).epsilon(0.002));
    CHECK(apparelDurability(*sim, pieces[3]) > 0.5f);  // the cowboy hat, 80 hit points: 70%
}

TEST_CASE("Apparel: a human's clothes are worn down and tainted when they die") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelSpawnHuman(*sim, 20, 20);
    APPAREL_REQUIRE(pawn != kNoEntity);
    const Entity shirt = apparelWear(*sim, pawn, "Apparel_TShirt");
    const Entity vest = apparelWear(*sim, pawn, "Apparel_FlakVest");
    const int vestMax = sim->things().registry().get<HitPoints>(vest).max;
    sim->submit(DamagePawnCommand{pawn, "Cut", 60.0f, 0});  // the torso
    sim->step();
    bool died = false;
    for (const Event& e : sim->events()) {
        died = died || (e.type == EventType::PawnDied && e.thing == pawn);
    }
    APPAREL_REQUIRE(died);
    CHECK(apparelHitPoints(*sim, shirt) == 100);
    CHECK(apparelHitPoints(*sim, vest) == vestMax);
    const float shirtDurability = apparelDurability(*sim, shirt);
    CHECK(shirtDurability <= 0.851f);  // 15% to 40% of what it had
    CHECK(shirtDurability >= 0.599f);
    CHECK(apparelDurability(*sim, vest) <= 0.851f);
    CHECK(apparelDurability(*sim, vest) >= 0.599f);
    CHECK(sim->things().registry().all_of<Tainted>(shirt));
    CHECK(sim->things().registry().all_of<Tainted>(vest));
    // Not again on later steps.
    sim->step();
    sim->step();
    CHECK(apparelDurability(*sim, shirt) == shirtDurability);

    // Something that isn't a person (no looks, no colonist record) taints nothing.
    const Entity beast = sim->things().spawn("Human", CellPos{30, 30, 0});
    const Entity coat = apparelWear(*sim, beast, "Apparel_TShirt");
    sim->submit(DamagePawnCommand{beast, "Cut", 60.0f, 0});
    sim->step();
    CHECK_FALSE(sim->things().registry().all_of<Tainted>(coat));
}

TEST_CASE("Apparel: thoughts about nakedness, nudists, tattered and dead men's clothes") {
    auto sim = apparelTestSim();
    // A man: naked until his legs are covered.
    const Entity man = apparelColonist(*sim, 20, 20);
    const ThoughtView* naked = apparelThought(apparelThoughts(*sim, man), "Naked");
    APPAREL_REQUIRE(naked != nullptr);
    CHECK(naked->offset == doctest::Approx(-6.0f));
    CHECK(apparelThought(apparelThoughts(*sim, man), "NudistNaked") == nullptr);
    const Entity pants = apparelWear(*sim, man, "Apparel_Pants");
    CHECK(apparelThought(apparelThoughts(*sim, man), "Naked") == nullptr);

    // A woman needs her chest covered too.
    const Entity woman = apparelColonist(*sim, 24, 20, true);
    apparelWear(*sim, woman, "Apparel_Pants");
    CHECK(apparelThought(apparelThoughts(*sim, woman), "Naked") != nullptr);
    apparelWear(*sim, woman, "Apparel_TShirt");
    CHECK(apparelThought(apparelThoughts(*sim, woman), "Naked") == nullptr);

    // A nudist: glad to be naked, unhappy dressed (a hat doesn't count).
    const Entity nudist = apparelColonist(*sim, 28, 20, false, "Nudist");
    const std::vector<ThoughtView> bare = apparelThoughts(*sim, nudist);
    CHECK(apparelThought(bare, "Naked") == nullptr);
    APPAREL_REQUIRE(apparelThought(bare, "NudistNaked") != nullptr);
    CHECK(apparelThought(bare, "NudistNaked")->offset == doctest::Approx(20.0f));
    // Ours (sneed's G2 playtest): a nudist is comfortable 10 C colder than anyone else with nothing on; the
    // hot end is the same.
    {
        const Entity plain = apparelColonist(*sim, 32, 20);
        float nudistMin = 0.0f, nudistMax = 0.0f, plainMin = 0.0f, plainMax = 0.0f;
        APPAREL_REQUIRE(sim->temperature().comfortRange(nudist, nudistMin, nudistMax));
        APPAREL_REQUIRE(sim->temperature().comfortRange(plain, plainMin, plainMax));
        CHECK(nudistMin == doctest::Approx(plainMin - 10.0f));
        CHECK(nudistMax == doctest::Approx(plainMax));
    }
    apparelWear(*sim, nudist, "Apparel_CowboyHat");
    CHECK(apparelThought(apparelThoughts(*sim, nudist), "NudistNaked") != nullptr);
    CHECK(apparelThought(apparelThoughts(*sim, nudist), "NudistWearingClothes") == nullptr);
    apparelWear(*sim, nudist, "Apparel_Pants");
    const std::vector<ThoughtView> dressed = apparelThoughts(*sim, nudist);
    CHECK(apparelThought(dressed, "NudistNaked") == nullptr);
    APPAREL_REQUIRE(apparelThought(dressed, "NudistWearingClothes") != nullptr);
    CHECK(apparelThought(dressed, "NudistWearingClothes")->offset == doctest::Approx(-3.0f));
    CHECK(apparelThought(apparelThoughts(*sim, man), "NudistWearingClothes") == nullptr);

    // Tattered only at 0 durability, and only -1; worn but not worn out is fine.
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel") == nullptr);
    apparelSetHitPoints(*sim, pants, 10);
    apparelSetDurability(*sim, pants, 0.05f);
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel") == nullptr);
    apparelSetDurability(*sim, pants, 0.0f);
    const ThoughtView* tattered = apparelThought(apparelThoughts(*sim, man), "TatteredApparel");
    APPAREL_REQUIRE(tattered != nullptr);
    CHECK(tattered->offset == doctest::Approx(-1.0f));
    // -1 for each tattered piece, up to -3 (sneed, 2026-11-14).
    const Entity worn1 = apparelWear(*sim, man, "Apparel_CowboyHat");
    const Entity worn2 = apparelWear(*sim, man, "Apparel_SimpleHelmet");
    const Entity worn3 = apparelWear(*sim, man, "Apparel_FlakVest");
    apparelSetDurability(*sim, worn1, 0.0f);
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel")->offset == doctest::Approx(-2.0f));
    apparelSetDurability(*sim, worn2, 0.0f);
    apparelSetDurability(*sim, worn3, 0.0f);
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel")->offset == doctest::Approx(-3.0f));
    for (const Entity w : {worn1, worn2, worn3}) {
        std::string error;
        CHECK(sim->apparel().takeOff(man, w, error));
    }
    sim->things().registry().remove<Durability>(pants);
    apparelSetHitPoints(*sim, pants, 100);
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel") == nullptr);

    // Dead men's clothes: -5, -8, -11, -14 by how many pieces; the bloodlusty don't mind.
    const Entity shirt = apparelWear(*sim, man, "Apparel_TShirt");
    sim->things().registry().emplace<Tainted>(shirt);
    CHECK(apparelThought(apparelThoughts(*sim, man), "DeadMansApparel")->offset == doctest::Approx(-5.0f));
    sim->things().registry().emplace<Tainted>(pants);
    CHECK(apparelThought(apparelThoughts(*sim, man), "DeadMansApparel")->offset == doctest::Approx(-8.0f));
    const Entity hat = apparelWear(*sim, man, "Apparel_CowboyHat");
    sim->things().registry().emplace<Tainted>(hat);
    const Entity duster = apparelWear(*sim, man, "Apparel_FlakVest");
    sim->things().registry().emplace<Tainted>(duster);
    CHECK(apparelThought(apparelThoughts(*sim, man), "DeadMansApparel")->offset == doctest::Approx(-14.0f));
    const Entity violent = apparelColonist(*sim, 32, 20, false, "Bloodlust");
    const Entity violentShirt = apparelWear(*sim, violent, "Apparel_TShirt");
    apparelWear(*sim, violent, "Apparel_Pants");
    sim->things().registry().emplace<Tainted>(violentShirt);
    CHECK(apparelThought(apparelThoughts(*sim, violent), "DeadMansApparel") == nullptr);

    // They count in the mood the pawn heads for.
    const float target = sim->mood().target(man, sim->time().seconds);
    sim->things().registry().remove<Tainted>(shirt);
    sim->things().registry().remove<Tainted>(pants);
    sim->things().registry().remove<Tainted>(hat);
    sim->things().registry().remove<Tainted>(duster);
    CHECK(sim->mood().target(man, sim->time().seconds) == doctest::Approx(target + 14.0f));
}

TEST_CASE("Apparel: a pawn puts on the clothes lying nearby, one piece at a time") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity shirt = apparelLying(*sim, 24, 20, "Apparel_TShirt");
    const Entity pants = apparelLying(*sim, 20, 25, "Apparel_Pants");
    // Out of reach (too far): left alone.
    const Entity faraway = apparelLying(*sim, 20, 90, "Apparel_Parka");
    APPAREL_REQUIRE(pawn != kNoEntity);
    int64_t done = -1;
    for (int i = 0; i < 1500 && done < 0; ++i) {
        sim->step();
        if (apparelWearing(*sim, pawn, shirt) && apparelWearing(*sim, pawn, pants)) {
            done = sim->time().seconds;
        }
    }
    CHECK(done > 0);
    CHECK(done < 600);  // two walks and two puttings on of 60 steps each
    CHECK_FALSE(sim->things().registry().all_of<Position>(shirt));
    CHECK(sim->things().spawned(faraway));
    CHECK(sim->apparel().worn(pawn).size() == 2);
    // It doesn't keep looking at once: no new job, and the check waits.
    for (int i = 0; i < 5; ++i) {
        sim->step();
    }
    CHECK(sim->things().registry().get<PawnOutfit>(pawn).nextCheck > sim->time().seconds);
}

TEST_CASE("Apparel: a pawn takes off a piece its outfit doesn't allow, and doesn't put it back") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity vest = apparelWear(*sim, pawn, "Apparel_FlakVest");
    const Entity shirt = apparelWear(*sim, pawn, "Apparel_TShirt");
    const uint32_t worker = apparelOutfitId(*sim, "Worker");
    APPAREL_REQUIRE(worker != 0);
    sim->submit(SetOutfitCommand{pawn, worker});
    sim->step();
    CHECK(sim->apparel().outfitOf(pawn) == worker);
    for (int i = 0; i < 400; ++i) {
        sim->step();
    }
    CHECK_FALSE(apparelWearing(*sim, pawn, vest));
    CHECK(apparelWearing(*sim, pawn, shirt));
    CHECK(sim->things().spawned(vest));  // on the ground where the pawn stood
    // Back to "Anything", it wears the vest again.
    sim->submit(SetOutfitCommand{pawn, apparelOutfitId(*sim, "Anything")});
    for (int i = 0; i < 600; ++i) {
        sim->step();
    }
    CHECK(apparelWearing(*sim, pawn, vest));
}

TEST_CASE("Apparel: outfits limit by def, hit points and quality, and can be made, edited and removed") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const uint32_t anything = apparelOutfitId(*sim, "Anything");
    const uint32_t soldier = apparelOutfitId(*sim, "Soldier");
    const uint32_t nudist = apparelOutfitId(*sim, "Nudist");
    APPAREL_REQUIRE(anything == 1);
    APPAREL_REQUIRE(soldier != 0);
    APPAREL_REQUIRE(nudist != 0);

    const Entity healthy = apparelLying(*sim, 40, 40, "Apparel_Pants");
    const Entity tired = apparelLying(*sim, 41, 40, "Apparel_Pants");
    apparelSetHitPoints(*sim, tired, 40);
    const OutfitPolicy* soldierPolicy = sim->apparel().outfit(soldier);
    APPAREL_REQUIRE(soldierPolicy != nullptr);
    CHECK(sim->apparel().allowed(*soldierPolicy, healthy));
    CHECK_FALSE(sim->apparel().allowed(*soldierPolicy, tired));  // under half its hit points
    CHECK(sim->apparel().allowed(*sim->apparel().outfit(anything), tired));

    // The nudist outfit allows hats and helmets only.
    const OutfitPolicy* nudistPolicy = sim->apparel().outfit(nudist);
    CHECK_FALSE(sim->apparel().allowed(*nudistPolicy, healthy));
    CHECK(sim->apparel().allowed(*nudistPolicy, apparelLying(*sim, 42, 40, "Apparel_CowboyHat")));
    CHECK(sim->apparel().allowed(*nudistPolicy, apparelLying(*sim, 43, 40, "Apparel_SimpleHelmet")));

    // A new outfit allows everything; edit it to refuse T-shirts and awful clothes.
    sim->submit(AddOutfitCommand{"No shirts"});
    sim->step();
    const uint32_t noShirts = apparelOutfitId(*sim, "No shirts");
    APPAREL_REQUIRE(noShirts == sim->apparel().nextOutfitId() - 1);
    EditOutfitCommand edit;
    edit.outfit = noShirts;
    edit.disallowed = {"Apparel_TShirt"};
    edit.qualityMin = 1;
    sim->submit(edit);
    sim->step();
    const OutfitPolicy* policy = sim->apparel().outfit(noShirts);
    APPAREL_REQUIRE(policy != nullptr);
    CHECK(sim->apparel().allowed(*policy, healthy));
    CHECK_FALSE(sim->apparel().allowed(*policy, apparelLying(*sim, 44, 40, "Apparel_TShirt")));
    CHECK_FALSE(sim->apparel().allowed(*policy, apparelLying(*sim, 45, 40, "Apparel_Pants", 0)));  // awful
    CHECK(sim->apparel().allowed(*policy, apparelLying(*sim, 46, 40, "Apparel_Pants", 1)));

    // Wrong edits and commands fail and change nothing.
    const auto failures = [&](Command command) {
        sim->submit(std::move(command));
        sim->step();
        int n = 0;
        for (const Event& e : sim->events()) {
            n += e.type == EventType::CommandFailed ? 1 : 0;
        }
        return n;
    };
    EditOutfitCommand bad = edit;
    bad.disallowed = {"Steel"};
    CHECK(failures(bad) == 1);
    bad = edit;
    bad.hitPointsMin = 0.8f;
    bad.hitPointsMax = 0.2f;
    CHECK(failures(bad) == 1);
    bad = edit;
    bad.qualityMax = 99;
    CHECK(failures(bad) == 1);
    CHECK(failures(SetOutfitCommand{pawn, 999}) == 1);
    CHECK(failures(SetOutfitCommand{healthy, anything}) == 1);  // an item, not a pawn
    CHECK(failures(RemoveOutfitCommand{anything}) == 1);
    CHECK(failures(AddOutfitCommand{""}) == 1);
    CHECK(sim->apparel().outfit(noShirts)->disallowed.size() == 1);

    // Pawns following a removed outfit go back to the default.
    CHECK(failures(SetOutfitCommand{pawn, noShirts}) == 0);
    CHECK(sim->apparel().outfitOf(pawn) == noShirts);
    CHECK(failures(RemoveOutfitCommand{noShirts}) == 0);
    CHECK(sim->apparel().outfit(noShirts) == nullptr);
    CHECK(sim->apparel().outfitOf(pawn) == anything);
}

TEST_CASE("Apparel: a worn piece that falls under the outfit's hit point range is taken off") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity pants = apparelWear(*sim, pawn, "Apparel_Pants");
    sim->submit(SetOutfitCommand{pawn, apparelOutfitId(*sim, "Soldier")});
    for (int i = 0; i < 300; ++i) {
        sim->step();
    }
    CHECK(apparelWearing(*sim, pawn, pants));  // in good shape
    apparelSetHitPoints(*sim, pants, 30);
    sim->things().registry().get<PawnOutfit>(pawn).nextCheck = 0;
    for (int i = 0; i < 300; ++i) {
        sim->step();
    }
    CHECK_FALSE(apparelWearing(*sim, pawn, pants));
}

TEST_CASE("Apparel: a colonist strips a dead pawn of clothes and weapon where it lies") {
    auto sim = apparelTestSim();
    const Entity victim = apparelSpawnHuman(*sim, 30, 30);
    APPAREL_REQUIRE(victim != kNoEntity);
    const Entity shirt = apparelWear(*sim, victim, "Apparel_TShirt");
    const Entity vest = apparelWear(*sim, victim, "Apparel_FlakVest");
    const Entity gun = sim->things().make(apparelDef(*sim, "Gun_Revolver"));
    sim->combat().equip(victim, gun);

    // A healthy pawn can't be stripped.
    sim->submit(StripCommand{victim, true});
    sim->step();
    int failed = 0;
    for (const Event& e : sim->events()) {
        failed += e.type == EventType::CommandFailed ? 1 : 0;
    }
    CHECK(failed == 1);

    sim->submit(DamagePawnCommand{victim, "Cut", 60.0f, 0});
    sim->step();
    sim->submit(StripCommand{victim, true});
    sim->step();
    CHECK(sim->apparel().stripMarked(victim));
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    const ThingView* view = snapshot.find(victim);
    APPAREL_REQUIRE(view != nullptr);
    CHECK(view->markedForStrip);
    CHECK(view->dead);

    // The colonist is a worker with Hauling on (priority 3).
    const Entity colonist = apparelColonist(*sim, 34, 30);
    apparelWear(*sim, colonist, "Apparel_TShirt");
    apparelWear(*sim, colonist, "Apparel_Pants");
    sim->submit(SetOutfitCommand{colonist, apparelOutfitId(*sim, "Nudist")});
    bool stripJob = false;
    int64_t done = -1;
    for (int i = 0; i < 1500 && done < 0; ++i) {
        sim->step();
        for (const Event& e : sim->events()) {
            stripJob = stripJob || (e.type == EventType::JobStarted && e.job == JobType::StripPawn && e.thing == colonist);
        }
        if (!sim->apparel().hasGear(victim)) {
            done = sim->time().seconds;
        }
    }
    CHECK(stripJob);
    CHECK(done > 0);
    CHECK_FALSE(sim->apparel().stripMarked(victim));
    CHECK(sim->things().spawned(shirt));
    CHECK(sim->things().spawned(vest));
    CHECK(sim->things().spawned(gun));
    CHECK(sim->things().registry().all_of<Tainted>(shirt));  // it was worn by a human as they died
    CHECK_FALSE(sim->things().registry().all_of<EquippedBy>(vest));
}

TEST_CASE("Apparel: the strip order goes when the pawn gets up or has nothing left to take") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 30, 30);
    apparelWear(*sim, pawn, "Apparel_TShirt");
    std::string error;
    CHECK_FALSE(sim->apparel().markStrip(pawn, true, error));  // not downed
    CHECK_FALSE(error.empty());
    sim->things().registry().get<Health>(pawn).downed = true;
    CHECK(sim->apparel().markStrip(pawn, true, error));
    CHECK(sim->apparel().stripMarked(pawn));
    sim->things().registry().get<Health>(pawn).downed = false;
    sim->things().registry().get<Health>(pawn).dead = false;
    sim->step();
    CHECK_FALSE(sim->apparel().stripMarked(pawn));  // it can act again
    CHECK(sim->apparel().markStrip(pawn, false, error));  // taking off a mark nobody put is fine
}

TEST_CASE("Apparel: a new game's colonists land dressed, except nudists") {
    SimSettings settings;
    settings.seed = 42;
    settings.mapSize = 128;
    settings.scenario = "Crashlanded";
    Sim sim(settings, apparelTestDefs());
    CHECK(sim.setupErrors().empty());
    int colonists = 0;
    for (const auto [pawn, colonist] : sim.things().registry().view<const Colonist>().each()) {
        ++colonists;
        const bool nudist = std::any_of(colonist.traits.begin(), colonist.traits.end(),
                                        [](const TraitRecord& t) { return t.def == "Nudist"; });
        if (nudist) {
            CHECK(sim.apparel().worn(pawn).empty());
            continue;
        }
        CHECK(sim.apparel().worn(pawn).size() == 2);
        CHECK(sim.apparel().covered(pawn, "Legs"));
        CHECK(sim.apparel().covered(pawn, "Torso"));
        for (const Entity piece : sim.apparel().worn(pawn)) {
            CHECK(sim.things().registry().get<MadeOf>(piece).stuff == apparelDef(sim, "Cloth"));
        }
    }
    CHECK(colonists == 6);
}

TEST_CASE("Apparel: the snapshot lists what each pawn wears and the outfits") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity shirt = apparelWear(*sim, pawn, "Apparel_TShirt", 3);
    const Entity pants = apparelWear(*sim, pawn, "Apparel_Pants");
    apparelSetHitPoints(*sim, pants, 50);
    sim->things().registry().emplace<Tainted>(pants);
    const Entity lying = apparelLying(*sim, 22, 22, "Apparel_Duster", 4, "DevilstrandCloth");
    Snapshot snapshot;
    sim->writeSnapshot(snapshot);
    APPAREL_REQUIRE(snapshot.outfits.size() >= 4);
    CHECK(snapshot.outfits[0].name == "Anything");
    const ThingView* view = snapshot.find(pawn);
    APPAREL_REQUIRE(view != nullptr);
    CHECK(view->outfit == 1);
    APPAREL_REQUIRE(view->apparelCount == 2);
    const ApparelView& first = snapshot.apparel[view->firstApparel];
    const ApparelView& second = snapshot.apparel[view->firstApparel + 1];
    CHECK(first.item == shirt);
    CHECK(first.quality == 3);
    CHECK(first.stuff == apparelDef(*sim, "Cloth"));
    CHECK_FALSE(first.tainted);
    CHECK(second.item == pants);
    CHECK(second.hitPointShare == doctest::Approx(0.5f));
    CHECK(second.tainted);
    const ThingView* item = snapshot.find(lying);
    APPAREL_REQUIRE(item != nullptr);
    CHECK(item->stuff == apparelDef(*sim, "DevilstrandCloth"));
    CHECK(item->quality == 4);
    CHECK(item->hitPoints == 260);
}

TEST_CASE("Apparel: saving and loading keeps stuff, quality, taint, outfits and strip orders") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    const Entity pants = apparelWear(*sim, pawn, "Apparel_Pants", 4);
    sim->things().registry().emplace<Tainted>(pants);
    apparelSetDurability(*sim, pants, 0.375f);
    const Entity lying = apparelLying(*sim, 22, 20, "Apparel_Duster", 1, "DevilstrandCloth");
    sim->submit(AddOutfitCommand{"Mine"});
    sim->step();
    EditOutfitCommand edit;
    edit.outfit = apparelOutfitId(*sim, "Mine");
    edit.disallowed = {"Apparel_Parka", "Apparel_Tuque"};
    edit.hitPointsMin = 0.25f;
    edit.qualityMin = 1;
    edit.qualityMax = 5;
    sim->submit(edit);
    sim->submit(SetOutfitCommand{pawn, edit.outfit});
    sim->step();
    const Entity dead = apparelSpawnHuman(*sim, 40, 40);
    APPAREL_REQUIRE(dead != kNoEntity);
    apparelWear(*sim, dead, "Apparel_TShirt");
    sim->submit(DamagePawnCommand{dead, "Cut", 60.0f, 0});
    sim->step();
    sim->submit(StripCommand{dead, true});
    for (int i = 0; i < 200; ++i) {
        sim->step();
    }

    const std::vector<uint8_t> bytes = saveSim(*sim);
    SaveLoadResult loaded = loadSim(bytes, apparelTestDefs());
    APPAREL_REQUIRE(loaded.error.empty());
    APPAREL_REQUIRE(loaded.sim != nullptr);
    Sim& copy = *loaded.sim;
    CHECK(copy.stateHash() == sim->stateHash());
    CHECK(saveSim(copy) == bytes);
    CHECK(copy.apparel().outfits().size() == sim->apparel().outfits().size());
    const OutfitPolicy* mine = copy.apparel().outfit(edit.outfit);
    APPAREL_REQUIRE(mine != nullptr);
    CHECK(mine->name == "Mine");
    CHECK(mine->disallowed.size() == 2);
    CHECK(mine->hitPointsMin == doctest::Approx(0.25f));
    CHECK(mine->qualityMin == 1);
    CHECK(mine->qualityMax == 5);
    CHECK(copy.apparel().outfitOf(pawn) == edit.outfit);
    CHECK(copy.things().registry().get<Quality>(pants).level == 4);
    CHECK(copy.things().registry().all_of<Tainted>(pants));
    CHECK(copy.things().registry().get<Durability>(pants).share == sim->things().registry().get<Durability>(pants).share);
    CHECK(copy.things().registry().get<MadeOf>(lying).stuff == apparelDef(copy, "DevilstrandCloth"));
    CHECK(copy.apparel().stripMarked(dead) == sim->apparel().stripMarked(dead));

    // Run both on: the same game.
    for (int i = 0; i < 3000; ++i) {
        sim->step();
        copy.step();
    }
    CHECK(copy.stateHash() == sim->stateHash());
}

TEST_CASE("Apparel: the clothing commands record into a replay and play back the same") {
    SimSettings settings;
    settings.seed = 5;
    settings.mapSize = 96;
    settings.flatMap = true;
    Sim original(settings, apparelTestDefs());
    original.submit(SpawnThingCommand{"Human", CellPos{20, 20, 0}, 1});
    original.submit(SpawnThingCommand{"Apparel_FlakVest", CellPos{22, 20, 0}, 1});
    original.submit(SpawnThingCommand{"Apparel_SimpleHelmet", CellPos{22, 22, 0}, 1});
    original.step();
    Entity pawn = kNoEntity;
    for (const Event& e : original.events()) {
        if (e.type == EventType::ThingSpawned && original.things().registry().get<Thing>(e.thing).category == ThingCategory::Pawn) {
            pawn = e.thing;
        }
    }
    APPAREL_REQUIRE(pawn != kNoEntity);
    original.submit(AddOutfitCommand{"Replayed"});
    original.step();
    EditOutfitCommand edit;
    edit.outfit = 5;
    edit.disallowed = {"Apparel_SimpleHelmet"};
    edit.hitPointsMin = 0.1f;
    edit.qualityMax = 4;
    original.submit(edit);
    original.submit(SetOutfitCommand{pawn, 5});
    original.submit(StripCommand{pawn, true});  // not downed: fails, and is logged
    for (int i = 0; i < 800; ++i) {
        original.step();
    }
    original.submit(RemoveOutfitCommand{5});
    original.step();

    const Replay recorded = recordReplay(original);
    std::string error;
    Replay parsed;
    APPAREL_REQUIRE(readReplay(writeReplay(recorded), parsed, error));
    CHECK(parsed.commands.size() == recorded.commands.size());
    Sim again(parsed.settings, apparelTestDefs());
    playReplay(parsed, again, recorded.endStep);
    CHECK(checkReplay(parsed, again).matched());
    CHECK(again.stateHash() == original.stateHash());

    // Every command type is known by name.
    for (const char* name : {"SetOutfit", "EditOutfit", "AddOutfit", "RemoveOutfit", "Strip"}) {
        Command command;
        CHECK(commandFromName(name, command));
        CHECK(std::string(commandName(command)) == name);
    }
}

TEST_CASE("Apparel: worn weight slows walking; armour slows attacks and, a quarter as much, work") {
    auto sim = apparelTestSim();
    const entt::registry& reg = sim->things().registry();
    const ApparelStats& stats = sim->apparel().stats();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(1.0f));
    // Cotton shirt and pants: 1 kg each, 2% slower, no slower to shoot or work.
    apparelWear(*sim, pawn, "Apparel_TShirt");
    apparelWear(*sim, pawn, "Apparel_Pants");
    CHECK(stats.wornKg(reg, pawn) == doctest::Approx(2.0f));
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(1.0f / 1.02f));
    CHECK(stats.attackTimeFactor(reg, pawn) == doctest::Approx(1.0f));
    CHECK(stats.workFactor(reg, pawn) == doctest::Approx(1.0f));
    // A cataphract suit (15 kg) and helmet (2 kg) over them; the tuque under the helmet weighs nothing.
    apparelWear(*sim, pawn, "Apparel_Tuque");
    apparelWear(*sim, pawn, "Apparel_CataphractArmor");
    apparelWear(*sim, pawn, "Apparel_CataphractHelmet");
    CHECK(stats.armourKg(reg, pawn) == doctest::Approx(17.0f));
    CHECK(stats.wornKg(reg, pawn) == doctest::Approx(19.0f));
    CHECK(stats.moveFactor(reg, pawn) == doctest::Approx(1.0f / 1.19f));
    CHECK(stats.attackTimeFactor(reg, pawn) == doctest::Approx(1.17f));
    CHECK(stats.workFactor(reg, pawn) == doctest::Approx(1.0f / (1.0f + 0.25f * 0.17f)));
    // The weapon in its hands counts toward walking (a 2 kg longsword).
    const Entity sword = sim->things().make(apparelDef(*sim, "Melee_Longsword"));
    APPAREL_REQUIRE(sim->combat().equip(pawn, sword));
    CHECK(stats.wornKg(reg, pawn) == doctest::Approx(21.0f));
    // What it wears and holds is not in its carry load (sneed: it only slows the walk).
    CHECK(sim->jobs().loadMass(pawn) == doctest::Approx(0.0));
}

TEST_CASE("Apparel: clothing goes on at once, armour takes 4 seconds, and a hit stops it") {
    for (const bool hit : {false, true}) {
        auto sim = apparelTestSim();
        const Entity pawn = apparelColonist(*sim, 20, 20);
        const Entity vest = apparelLying(*sim, 20, 20, "Apparel_FlakVest");
        CHECK(sim->apparel().wearStepsFor(vest) == 120);
        CHECK(sim->apparel().wearStepsFor(sim->things().make(apparelDef(*sim, "Apparel_TShirt"))) == 1);
        int64_t started = -1;
        int64_t worn = -1;
        for (int i = 0; i < 400 && worn < 0; ++i) {
            sim->step();
            const Worker& w = sim->things().registry().get<Worker>(pawn);
            if (started < 0 && w.job.type == JobType::WearApparel && w.job.thing == vest && w.job.progress > 0.0f) {
                started = sim->time().seconds;
                if (hit) {
                    sim->step();
                    sim->submit(DamagePawnCommand{pawn, "Cut", 2.0f, 0});
                }
            }
            if (apparelWearing(*sim, pawn, vest)) {
                worn = sim->time().seconds;
            }
            if (hit && started >= 0 && sim->time().seconds >= started + 121) {
                break;
            }
        }
        APPAREL_REQUIRE(started >= 0);
        if (hit) {
            CHECK(worn < 0);  // the hit stopped it
        } else {
            CHECK(worn - started >= 118);
            CHECK(worn - started <= 124);
        }
    }
}

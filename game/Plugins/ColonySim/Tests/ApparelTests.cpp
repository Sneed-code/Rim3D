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
#include "ColonySim/Snapshot.h"
#include "ColonySim/Things.h"

#include <doctest.h>

#include <algorithm>
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

TEST_CASE("Apparel: armor, insulation and hit points come from the stuff and the quality") {
    auto sim = apparelTestSim();
    const ApparelStats& stats = sim->apparel().stats();
    const entt::registry& reg = sim->things().registry();

    // A parka of cloth: 2.0 x cloth's 18 degrees of cold; none against heat.
    const Entity parka = apparelLying(*sim, 10, 10, "Apparel_Parka");
    CHECK(stats.insulationCold(reg, parka) == doctest::Approx(36.0f));
    CHECK(stats.insulationHeat(reg, parka) == doctest::Approx(0.0f));
    const Entity good = apparelLying(*sim, 11, 10, "Apparel_Parka", 3);
    CHECK(stats.insulationCold(reg, good) == doctest::Approx(36.0f * 1.1f));
    CHECK(stats.qualityOf(reg, parka) == 2);
    CHECK(stats.qualityOf(reg, good) == 3);

    // A devilstrand duster: 0.3 x 1.4 sharp, 200 hit points x 1.3.
    const Entity duster = apparelLying(*sim, 12, 10, "Apparel_Duster", -1, "DevilstrandCloth");
    CHECK(stats.armor(reg, duster, ArmorType::Sharp) == doctest::Approx(0.42f));
    CHECK(sim->things().registry().get<HitPoints>(duster).max == 260);
    CHECK(apparelHitPoints(*sim, duster) == 260);
    const Entity master = apparelLying(*sim, 13, 10, "Apparel_Duster", 5, "DevilstrandCloth");
    CHECK(stats.armor(reg, master, ArmorType::Sharp) == doctest::Approx(0.42f * 1.45f));

    // A T-shirt of cloth stops next to nothing; a flak vest keeps its own rating.
    const Entity shirt = apparelLying(*sim, 14, 10, "Apparel_TShirt");
    CHECK(stats.armor(reg, shirt, ArmorType::Sharp) == doctest::Approx(0.2f * 0.36f));
    const Entity vest = apparelLying(*sim, 15, 10, "Apparel_FlakVest");
    CHECK(stats.armor(reg, vest, ArmorType::Sharp) == doctest::Approx(1.0f));
    CHECK(apparelHitPoints(*sim, vest) == 200);

    // A garment with no stuff given is worth nothing.
    const Entity bare = sim->things().make(apparelDef(*sim, "Apparel_Tribalwear"));
    CHECK(stats.armor(reg, bare, ArmorType::Sharp) == 0.0f);
    CHECK(stats.insulationCold(reg, bare) == 0.0f);

    // Things that aren't apparel are refused.
    std::string why;
    CHECK(sim->apparel().make(apparelDef(*sim, "Steel"), DefId{}, -1, CellPos{16, 10, 0}, &why) == kNoEntity);
    CHECK(sim->apparel().make(apparelDef(*sim, "Apparel_Parka"), apparelDef(*sim, "Cloth"), 9, CellPos{16, 10, 0}, &why) == kNoEntity);
}

TEST_CASE("Apparel: what a pawn wears adds up its insulation and layers as RimWorld's rules say") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    apparelWear(*sim, pawn, "Apparel_TShirt");
    apparelWear(*sim, pawn, "Apparel_Pants");
    apparelWear(*sim, pawn, "Apparel_Parka");
    float cold = 0.0f;
    float heat = 0.0f;
    sim->apparel().insulation(pawn, cold, heat);
    // shirt 0.22 x 18, pants 0.2 x 18, parka 2.0 x 18 (cold); 0.1 x 18, 0.08 x 18, 0 (heat)
    CHECK(cold == doctest::Approx(0.22f * 18 + 0.2f * 18 + 2.0f * 18));
    CHECK(heat == doctest::Approx(0.1f * 18 + 0.08f * 18));
    CHECK(sim->apparel().worn(pawn).size() == 3);

    // A jacket is also a Shell over the torso: it replaces the parka, which drops where the pawn stands.
    const Entity parka = sim->apparel().worn(pawn)[2];
    const Entity jacket = apparelWear(*sim, pawn, "Apparel_Jacket");
    CHECK(sim->apparel().worn(pawn).size() == 3);
    CHECK_FALSE(apparelWearing(*sim, pawn, parka));
    CHECK(sim->things().spawned(parka));
    CHECK(apparelWearing(*sim, pawn, jacket));
    CHECK(sim->apparel().covered(pawn, "Torso"));
    CHECK(sim->apparel().covered(pawn, "Legs"));
    CHECK_FALSE(sim->apparel().covered(pawn, "UpperHead"));
}

TEST_CASE("Apparel: armor wears down as it takes hits, and a piece at no hit points is gone") {
    auto sim = apparelTestSim();
    const Entity target = apparelColonist(*sim, 18, 30);
    const Entity vest = apparelWear(*sim, target, "Apparel_FlakVest");
    const Entity shirt = apparelWear(*sim, target, "Apparel_TShirt");
    CombatSystem& combat = sim->combat();

    // Damage direct: 5 points off, then enough to destroy it.
    combat.damageApparel(target, shirt, 5);
    CHECK(apparelHitPoints(*sim, shirt) == 95);
    sim->step();
    combat.damageApparel(target, shirt, 500);
    bool destroyedEvent = false;
    for (const Event& e : sim->events()) {
        destroyedEvent = destroyedEvent || (e.type == EventType::ApparelDestroyed && e.thing == target);
    }
    CHECK(destroyedEvent);
    CHECK_FALSE(sim->things().exists(shirt));
    CHECK(sim->apparel().worn(target).size() == 1);

    // Shot at: the vest takes 1 hit point for every 4 points of damage it absorbs, rounded at random.
    const Entity shooter = sim->things().spawn("Human", CellPos{10, 30, 0});
    combat.equip(shooter, sim->things().make(apparelDef(*sim, "Gun_Revolver")));
    Rng rng(4);
    int armorEvents = 0;
    bool vestGone = false;
    int lostBeforeGone = 0;
    for (int shot = 0; shot < 400 && !vestGone; ++shot) {
        const int before = apparelHitPoints(*sim, vest);
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
        if (!vestGone) {
            lostBeforeGone += before - apparelHitPoints(*sim, vest);
            CHECK(before - apparelHitPoints(*sim, vest) <= 8);  // one revolver bullet is about 12 damage
        }
        sim->health().refresh(target);
        sim->things().registry().replace<Health>(target, Health{});
        sim->health().refresh(target);
    }
    CHECK(armorEvents > 10);
    CHECK(lostBeforeGone > 0);
    CHECK(vestGone);
    CHECK_FALSE(sim->things().exists(vest));
    CHECK(sim->apparel().worn(target).empty());
}

TEST_CASE("Apparel: cloth stops nothing against a revolver, a devilstrand duster stops some") {
    for (const bool duster : {false, true}) {
        auto sim = apparelTestSim();
        const Entity target = apparelColonist(*sim, 18, 30);
        if (duster) {
            apparelWear(*sim, target, "Apparel_Duster", -1, "DevilstrandCloth");
        } else {
            apparelWear(*sim, target, "Apparel_TShirt");
        }
        const Entity shooter = sim->things().spawn("Human", CellPos{10, 30, 0});
        sim->combat().equip(shooter, sim->things().make(apparelDef(*sim, "Gun_Revolver")));
        Rng rng(8);
        int armorEvents = 0;
        for (int shot = 0; shot < 120; ++shot) {
            sim->combat().fire(shooter, 18.5f, 30.5f, 1.15f, 0.0f, rng);
            for (int s = 0; s < 20; ++s) {
                sim->step();
                for (const Event& e : sim->events()) {
                    armorEvents += e.type == EventType::ProjectileHit && e.hit == ProjectileHitKind::Armor ? 1 : 0;
                }
                if (sim->things().registry().view<Projectile>().empty()) {
                    break;
                }
            }
            sim->things().registry().replace<Health>(target, Health{});
            sim->health().refresh(target);
            const std::vector<Entity>& worn = sim->apparel().worn(target);
            for (const Entity piece : worn) {
                apparelSetHitPoints(*sim, piece, sim->things().registry().get<HitPoints>(piece).max);
            }
        }
        if (duster) {
            CHECK(armorEvents > 0);  // 42% less the revolver's 18%: about 12% of torso hits bounce
        } else {
            CHECK(armorEvents == 0);
        }
    }
}

TEST_CASE("Apparel: worn clothes lose hit points slowly, about 0.4 a day each") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelColonist(*sim, 20, 20);
    std::vector<Entity> pieces = {apparelWear(*sim, pawn, "Apparel_TShirt"), apparelWear(*sim, pawn, "Apparel_Pants"),
                                  apparelWear(*sim, pawn, "Apparel_Duster"), apparelWear(*sim, pawn, "Apparel_CowboyHat")};
    int before = 0;
    for (const Entity p : pieces) {
        before += apparelHitPoints(*sim, p);
    }
    Rng rng(3);
    const std::vector<Event> none;
    for (int hour = 0; hour < 24 * 20; ++hour) {
        sim->apparel().step(static_cast<int64_t>(hour) * 3600, rng, none);
    }
    int after = 0;
    for (const Entity p : pieces) {
        after += apparelHitPoints(*sim, p);
    }
    // 4 pieces x 20 days x 0.4 = 32 hit points, give or take chance.
    CHECK(before - after >= 15);
    CHECK(before - after <= 55);
    // Not at all between the hours.
    const int now = after;
    for (int i = 0; i < 50; ++i) {
        sim->apparel().step(3600 * 1000 + 1 + i, rng, none);
    }
    int later = 0;
    for (const Entity p : pieces) {
        later += apparelHitPoints(*sim, p);
    }
    CHECK(later == now);
}

TEST_CASE("Apparel: a human's clothes lose hit points and are tainted when they die") {
    auto sim = apparelTestSim();
    const Entity pawn = apparelSpawnHuman(*sim, 20, 20);
    APPAREL_REQUIRE(pawn != kNoEntity);
    const Entity shirt = apparelWear(*sim, pawn, "Apparel_TShirt");
    const Entity vest = apparelWear(*sim, pawn, "Apparel_FlakVest");
    sim->submit(DamagePawnCommand{pawn, "Cut", 60.0f, 0});  // the torso
    sim->step();
    bool died = false;
    for (const Event& e : sim->events()) {
        died = died || (e.type == EventType::PawnDied && e.thing == pawn);
    }
    APPAREL_REQUIRE(died);
    const int shirtHp = apparelHitPoints(*sim, shirt);
    const int vestHp = apparelHitPoints(*sim, vest);
    CHECK(shirtHp <= 85);  // 15% to 40% of what it had
    CHECK(shirtHp >= 60);
    CHECK(vestHp <= 170);
    CHECK(vestHp >= 120);
    CHECK(sim->things().registry().all_of<Tainted>(shirt));
    CHECK(sim->things().registry().all_of<Tainted>(vest));
    // Not again on later steps.
    sim->step();
    sim->step();
    CHECK(apparelHitPoints(*sim, shirt) == shirtHp);

    // Something that isn't a person (no looks, no colonist record) taints nothing.
    const Entity beast = sim->things().spawn("Human", CellPos{30, 30, 0});
    const Entity coat = apparelWear(*sim, beast, "Apparel_TShirt");
    sim->submit(DamagePawnCommand{beast, "Cut", 60.0f, 0});
    sim->step();
    CHECK_FALSE(sim->things().registry().all_of<Tainted>(coat));
}

TEST_CASE("Apparel: thoughts about nakedness, nudists, ratty and dead men's clothes") {
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
    apparelWear(*sim, nudist, "Apparel_CowboyHat");
    CHECK(apparelThought(apparelThoughts(*sim, nudist), "NudistNaked") != nullptr);
    CHECK(apparelThought(apparelThoughts(*sim, nudist), "NudistWearingClothes") == nullptr);
    apparelWear(*sim, nudist, "Apparel_Pants");
    const std::vector<ThoughtView> dressed = apparelThoughts(*sim, nudist);
    CHECK(apparelThought(dressed, "NudistNaked") == nullptr);
    APPAREL_REQUIRE(apparelThought(dressed, "NudistWearingClothes") != nullptr);
    CHECK(apparelThought(dressed, "NudistWearingClothes")->offset == doctest::Approx(-3.0f));
    CHECK(apparelThought(apparelThoughts(*sim, man), "NudistWearingClothes") == nullptr);

    // Ratty at half its hit points, tattered under a quarter.
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel") == nullptr);
    apparelSetHitPoints(*sim, pants, 50);
    const ThoughtView* ratty = apparelThought(apparelThoughts(*sim, man), "TatteredApparel");
    APPAREL_REQUIRE(ratty != nullptr);
    CHECK(ratty->offset == doctest::Approx(-3.0f));
    apparelSetHitPoints(*sim, pants, 24);
    const ThoughtView* tattered = apparelThought(apparelThoughts(*sim, man), "TatteredApparel");
    APPAREL_REQUIRE(tattered != nullptr);
    CHECK(tattered->offset == doctest::Approx(-5.0f));
    apparelSetHitPoints(*sim, pants, 51);
    CHECK(apparelThought(apparelThoughts(*sim, man), "TatteredApparel") == nullptr);

    // Dead men's clothes: -5, -8, -11, -14 by how many pieces; the bloodlusty don't mind.
    const Entity shirt = apparelWear(*sim, man, "Apparel_TShirt");
    sim->things().registry().emplace<Tainted>(shirt);
    CHECK(apparelThought(apparelThoughts(*sim, man), "DeadMansApparel")->offset == doctest::Approx(-5.0f));
    sim->things().registry().emplace<Tainted>(pants);
    CHECK(apparelThought(apparelThoughts(*sim, man), "DeadMansApparel")->offset == doctest::Approx(-8.0f));
    const Entity hat = apparelWear(*sim, man, "Apparel_CowboyHat");
    sim->things().registry().emplace<Tainted>(hat);
    const Entity duster = apparelWear(*sim, man, "Apparel_Duster");
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

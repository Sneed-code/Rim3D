// Small helpers for the bridge types: commands, events and the snapshot.
#include "ColonySim/Commands.h"
#include "ColonySim/Events.h"
#include "ColonySim/Snapshot.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace colony {

const char* commandName(const Command& command) {
    return std::visit(
        [](const auto& c) -> const char* {
            using Type = std::decay_t<decltype(c)>;
            if constexpr (std::is_same_v<Type, SpawnThingCommand>) {
                return "SpawnThing";
            } else if constexpr (std::is_same_v<Type, DestroyThingCommand>) {
                return "DestroyThing";
            } else if constexpr (std::is_same_v<Type, DamagePawnCommand>) {
                return "DamagePawn";
            } else if constexpr (std::is_same_v<Type, TendPawnCommand>) {
                return "TendPawn";
            } else if constexpr (std::is_same_v<Type, SetWorkPriorityCommand>) {
                return "SetWorkPriority";
            } else if constexpr (std::is_same_v<Type, CreateStockpileCommand>) {
                return "CreateStockpile";
            } else if constexpr (std::is_same_v<Type, RemoveZoneCellsCommand>) {
                return "RemoveZoneCells";
            } else if constexpr (std::is_same_v<Type, SetStockpilePriorityCommand>) {
                return "SetStockpilePriority";
            } else if constexpr (std::is_same_v<Type, SetStockpileAllowedCommand>) {
                return "SetStockpileAllowed";
            } else if constexpr (std::is_same_v<Type, OrderHaulCommand>) {
                return "OrderHaul";
            } else if constexpr (std::is_same_v<Type, ControlPawnCommand>) {
                return "ControlPawn";
            } else if constexpr (std::is_same_v<Type, DirectMoveCommand>) {
                return "DirectMove";
            } else if constexpr (std::is_same_v<Type, SetScheduleCommand>) {
                return "SetSchedule";
            } else if constexpr (std::is_same_v<Type, AssignBedCommand>) {
                return "AssignBed";
            } else if constexpr (std::is_same_v<Type, PlaceBlueprintCommand>) {
                return "PlaceBlueprint";
            } else if constexpr (std::is_same_v<Type, CancelBuildCommand>) {
                return "CancelBuild";
            } else if constexpr (std::is_same_v<Type, DeconstructCommand>) {
                return "Deconstruct";
            } else if constexpr (std::is_same_v<Type, EquipCommand>) {
                return "Equip";
            } else if constexpr (std::is_same_v<Type, ShootCommand>) {
                return "Shoot";
            } else if constexpr (std::is_same_v<Type, SetStanceCommand>) {
                return "SetStance";
            } else if constexpr (std::is_same_v<Type, DesignateCommand>) {
                return "Designate";
            } else if constexpr (std::is_same_v<Type, CancelDesignationsCommand>) {
                return "CancelDesignations";
            } else if constexpr (std::is_same_v<Type, DraftCommand>) {
                return "Draft";
            } else if constexpr (std::is_same_v<Type, MoveOrderCommand>) {
                return "MoveOrder";
            } else if constexpr (std::is_same_v<Type, AttackOrderCommand>) {
                return "AttackOrder";
            } else if constexpr (std::is_same_v<Type, SetFactionCommand>) {
                return "SetFaction";
            } else if constexpr (std::is_same_v<Type, SetLineageCommand>) {
                return "SetLineage";
            } else if constexpr (std::is_same_v<Type, ReloadCommand>) {
                return "Reload";
            } else if constexpr (std::is_same_v<Type, AddBillCommand>) {
                return "AddBill";
            } else if constexpr (std::is_same_v<Type, RemoveBillCommand>) {
                return "RemoveBill";
            } else if constexpr (std::is_same_v<Type, SetBillCommand>) {
                return "SetBill";
            } else if constexpr (std::is_same_v<Type, HandsOnCommand>) {
                return "HandsOn";
            } else if constexpr (std::is_same_v<Type, FlickCommand>) {
                return "Flick";
            } else if constexpr (std::is_same_v<Type, PlaceFloorCommand>) {
                return "PlaceFloor";
            } else if constexpr (std::is_same_v<Type, AddFilthCommand>) {
                return "AddFilth";
            } else if constexpr (std::is_same_v<Type, SetRefuelCommand>) {
                return "SetRefuel";
            } else if constexpr (std::is_same_v<Type, SetBillOptionsCommand>) {
                return "SetBillOptions";
            } else if constexpr (std::is_same_v<Type, MoveBillCommand>) {
                return "MoveBill";
            } else if constexpr (std::is_same_v<Type, ShortCircuitCommand>) {
                return "ShortCircuit";
            } else if constexpr (std::is_same_v<Type, SetResearchCommand>) {
                return "SetResearch";
            } else if constexpr (std::is_same_v<Type, UnlockResearchCommand>) {
                return "UnlockResearch";
            } else if constexpr (std::is_same_v<Type, ThrowCommand>) {
                return "Throw";
            } else if constexpr (std::is_same_v<Type, ExplodeCommand>) {
                return "Explode";
            } else if constexpr (std::is_same_v<Type, StartRaidCommand>) {
                return "StartRaid";
            } else if constexpr (std::is_same_v<Type, SoundAlarmCommand>) {
                return "SoundAlarm";
            } else if constexpr (std::is_same_v<Type, SetDefenseRoleCommand>) {
                return "SetDefenseRole";
            } else if constexpr (std::is_same_v<Type, MarkShelterCommand>) {
                return "MarkShelter";
            } else if constexpr (std::is_same_v<Type, MarkAnimalCommand>) {
                return "MarkAnimal";
            } else if constexpr (std::is_same_v<Type, MarkAnimalsInAreaCommand>) {
                return "MarkAnimalsInArea";
            } else if constexpr (std::is_same_v<Type, SetAnimalNameCommand>) {
                return "SetAnimalName";
            } else if constexpr (std::is_same_v<Type, SetTrainingCommand>) {
                return "SetTraining";
            } else if constexpr (std::is_same_v<Type, CreateAnimalZoneCommand>) {
                return "CreateAnimalZone";
            } else if constexpr (std::is_same_v<Type, RemoveAnimalZoneCellsCommand>) {
                return "RemoveAnimalZoneCells";
            } else if constexpr (std::is_same_v<Type, AssignAnimalZoneCommand>) {
                return "AssignAnimalZone";
            } else if constexpr (std::is_same_v<Type, SetBreedingCommand>) {
                return "SetBreeding";
            } else if constexpr (std::is_same_v<Type, MeleeCommand>) {
                return "Melee";
            } else if constexpr (std::is_same_v<Type, DodgeRollCommand>) {
                return "DodgeRoll";
            } else if constexpr (std::is_same_v<Type, SetOutfitCommand>) {
                return "SetOutfit";
            } else if constexpr (std::is_same_v<Type, EditOutfitCommand>) {
                return "EditOutfit";
            } else if constexpr (std::is_same_v<Type, AddOutfitCommand>) {
                return "AddOutfit";
            } else if constexpr (std::is_same_v<Type, RemoveOutfitCommand>) {
                return "RemoveOutfit";
            } else if constexpr (std::is_same_v<Type, StripCommand>) {
                return "Strip";
            } else if constexpr (std::is_same_v<Type, InteractCommand>) {
                return "Interact";
            } else if constexpr (std::is_same_v<Type, SetWeatherCommand>) {
                return "SetWeather";
            } else if constexpr (std::is_same_v<Type, SpendTalentPointCommand>) {
                return "SpendTalentPoint";
            } else if constexpr (std::is_same_v<Type, RearmTrapCommand>) {
                return "RearmTrap";
            } else if constexpr (std::is_same_v<Type, CreateGrowingZoneCommand>) {
                return "CreateGrowingZone";
            } else if constexpr (std::is_same_v<Type, RemoveGrowingZoneCellsCommand>) {
                return "RemoveGrowingZoneCells";
            } else if constexpr (std::is_same_v<Type, SetGrowingZoneCropCommand>) {
                return "SetGrowingZoneCrop";
            } else if constexpr (std::is_same_v<Type, AddTalentXpCommand>) {
                return "AddTalentXp";
            } else if constexpr (std::is_same_v<Type, CarryWeaponCommand>) {
                return "CarryWeapon";
            } else if constexpr (std::is_same_v<Type, SetNarratorCommand>) {
                return "SetNarrator";
            } else if constexpr (std::is_same_v<Type, FireIncidentCommand>) {
                return "FireIncident";
            } else if constexpr (std::is_same_v<Type, SwitchWeaponCommand>) {
                return "SwitchWeapon";
            } else if constexpr (std::is_same_v<Type, IgniteCommand>) {
                return "Ignite";
            } else if constexpr (std::is_same_v<Type, SetHomeAreaCommand>) {
                return "SetHomeArea";
            } else if constexpr (std::is_same_v<Type, SetMedicalCareCommand>) {
                return "SetMedicalCare";
            } else if constexpr (std::is_same_v<Type, SetPrisonerModeCommand>) {
                return "SetPrisonerMode";
            } else if constexpr (std::is_same_v<Type, SetPrisonBedCommand>) {
                return "SetPrisonBed";
            } else if constexpr (std::is_same_v<Type, PlaceUpperFloorCommand>) {
                return "PlaceUpperFloor";
            } else if constexpr (std::is_same_v<Type, StartMentalBreakCommand>) {
                return "StartMentalBreak";
            } else if constexpr (std::is_same_v<Type, ContractDiseaseCommand>) {
                return "ContractDisease";
            } else if constexpr (std::is_same_v<Type, EndMentalBreakCommand>) {
                return "EndMentalBreak";
            } else if constexpr (std::is_same_v<Type, StartStrategyRaidCommand>) {
                return "StartStrategyRaid";
            } else if constexpr (std::is_same_v<Type, ShipCommand>) {
                return "Ship";
            } else if constexpr (std::is_same_v<Type, QuestCommand>) {
                return "Quest";
            } else if constexpr (std::is_same_v<Type, SkipNightCommand>) {
                return "SkipNight";
            } else if constexpr (std::is_same_v<Type, FastTravelCommand>) {
                return "FastTravel";
            } else {
                static_assert(std::is_same_v<Type, CancelSkipCommand>, "name every command type");
                return "CancelSkip";
            }
        },
        command);
}

namespace {

// Tries each command type in turn. Prefixed because Unreal's unity builds merge .cpp files.
template <size_t I = 0>
bool bridgeCommandFromName(const std::string& name, Command& out) {
    if constexpr (I < std::variant_size_v<Command>) {
        Command blank{std::in_place_index<I>};
        if (name == commandName(blank)) {
            out = std::move(blank);
            return true;
        }
        return bridgeCommandFromName<I + 1>(name, out);
    } else {
        return false;
    }
}

}  // namespace

bool commandFromName(const std::string& name, Command& out) {
    return bridgeCommandFromName(name, out);
}

const char* eventTypeName(EventType type) {
    switch (type) {
        case EventType::ThingSpawned: return "ThingSpawned";
        case EventType::ThingDespawned: return "ThingDespawned";
        case EventType::ThingDestroyed: return "ThingDestroyed";
        case EventType::CommandFailed: return "CommandFailed";
        case EventType::PawnInjured: return "PawnInjured";
        case EventType::PawnPartLost: return "PawnPartLost";
        case EventType::PawnDowned: return "PawnDowned";
        case EventType::PawnGotUp: return "PawnGotUp";
        case EventType::PawnDied: return "PawnDied";
        case EventType::JobStarted: return "JobStarted";
        case EventType::JobEnded: return "JobEnded";
        case EventType::PawnAction: return "PawnAction";
        case EventType::PawnAte: return "PawnAte";
        case EventType::PawnWokeUp: return "PawnWokeUp";
        case EventType::CellMined: return "CellMined";
        case EventType::PlantRemoved: return "PlantRemoved";
        case EventType::PlantHarvested: return "PlantHarvested";
        case EventType::DesignationsChanged: return "DesignationsChanged";
        case EventType::FrameStarted: return "FrameStarted";
        case EventType::PawnMoodBand: return "PawnMoodBand";
        case EventType::ShotFired: return "ShotFired";
        case EventType::ProjectileHit: return "ProjectileHit";
        case EventType::RecipeDone: return "RecipeDone";
        case EventType::PowerChanged: return "PowerChanged";
        case EventType::ShortCircuit: return "ShortCircuit";
        case EventType::ItemSpoiled: return "ItemSpoiled";
        case EventType::PawnPoisoned: return "PawnPoisoned";
        case EventType::ResearchDone: return "ResearchDone";
        case EventType::GrenadeThrown: return "GrenadeThrown";
        case EventType::GrenadeLanded: return "GrenadeLanded";
        case EventType::Explosion: return "Explosion";
        case EventType::PawnKnockedDown: return "PawnKnockedDown";
        case EventType::RaidArrived: return "RaidArrived";
        case EventType::SquadChanged: return "SquadChanged";
        case EventType::AlarmChanged: return "AlarmChanged";
        case EventType::PawnLeftMap: return "PawnLeftMap";
        case EventType::DropPodLanded: return "DropPodLanded";
        case EventType::SappersBreaking: return "SappersBreaking";
        case EventType::ItemStolen: return "ItemStolen";
        case EventType::PawnKidnapped: return "PawnKidnapped";
        case EventType::RoofCollapsed: return "RoofCollapsed";
        case EventType::AnimalTamed: return "AnimalTamed";
        case EventType::AnimalReleased: return "AnimalReleased";
        case EventType::AnimalTrained: return "AnimalTrained";
        case EventType::AnimalAttemptFailed: return "AnimalAttemptFailed";
        case EventType::AnimalSlaughtered: return "AnimalSlaughtered";
        case EventType::AnimalStarved: return "AnimalStarved";
        case EventType::MeleeSwing: return "MeleeSwing";
        case EventType::MeleeHit: return "MeleeHit";
        case EventType::PawnTended: return "PawnTended";
        case EventType::PawnScarred: return "PawnScarred";
        case EventType::PawnSick: return "PawnSick";
        case EventType::PawnRecovered: return "PawnRecovered";
        case EventType::ApparelDestroyed: return "ApparelDestroyed";
        case EventType::WeatherChanged: return "WeatherChanged";
        case EventType::SeasonChanged: return "SeasonChanged";
        case EventType::TalentPoint: return "TalentPoint";
        case EventType::AnimalMated: return "AnimalMated";
        case EventType::AnimalBorn: return "AnimalBorn";
        case EventType::EggLaid: return "EggLaid";
        case EventType::EggHatched: return "EggHatched";
        case EventType::AnimalGathered: return "AnimalGathered";
        case EventType::TrapSprung: return "TrapSprung";
        case EventType::TrapRearmed: return "TrapRearmed";
        case EventType::WeaponSwitched: return "WeaponSwitched";
        case EventType::PlantSown: return "PlantSown";
        case EventType::Letter: return "Letter";
        case EventType::FireStarted: return "FireStarted";
        case EventType::FireOut: return "FireOut";
        case EventType::PawnCaughtFire: return "PawnCaughtFire";
        case EventType::PawnFireOut: return "PawnFireOut";
        case EventType::PrisonerCaptured: return "PrisonerCaptured";
        case EventType::PrisonerChatted: return "PrisonerChatted";
        case EventType::PrisonerRecruited: return "PrisonerRecruited";
        case EventType::PrisonerReleased: return "PrisonerReleased";
        case EventType::PrisonBreak: return "PrisonBreak";
        case EventType::Interaction: return "Interaction";
        case EventType::BondChanged: return "BondChanged";
        case EventType::SocialFight: return "SocialFight";
        case EventType::OutOfFuel: return "OutOfFuel";
        case EventType::Refueled: return "Refueled";
        case EventType::FirefoamPopped: return "FirefoamPopped";
        case EventType::AnimalHunted: return "AnimalHunted";
        case EventType::AnimalButchered: return "AnimalButchered";
        case EventType::PawnMentalBreak: return "PawnMentalBreak";
        case EventType::PawnMentalBreakEnded: return "PawnMentalBreakEnded";
        case EventType::PawnInsulted: return "PawnInsulted";
        case EventType::PawnLeftColony: return "PawnLeftColony";
        case EventType::QuestOffered: return "QuestOffered";
        case EventType::QuestAccepted: return "QuestAccepted";
        case EventType::QuestDeclined: return "QuestDeclined";
        case EventType::QuestExpired: return "QuestExpired";
        case EventType::QuestCompleted: return "QuestCompleted";
        case EventType::QuestFailed: return "QuestFailed";
        case EventType::InspirationStarted: return "InspirationStarted";
        case EventType::InspirationEnded: return "InspirationEnded";
        case EventType::ShipReactorStarted: return "ShipReactorStarted";
        case EventType::ShipWave: return "ShipWave";
        case EventType::ShipReactorReady: return "ShipReactorReady";
        case EventType::ShipReactorLost: return "ShipReactorLost";
        case EventType::ShipLaunched: return "ShipLaunched";
        case EventType::SkipStarted: return "SkipStarted";
        case EventType::SkipEnded: return "SkipEnded";
    }
    return "?";
}

const char* pawnActionName(PawnAction action) {
    switch (action) {
        case PawnAction::PickUp: return "PickUp";
        case PawnAction::PutDown: return "PutDown";
        case PawnAction::Eat: return "Eat";
        case PawnAction::LieDown: return "LieDown";
        case PawnAction::GetUp: return "GetUp";
        case PawnAction::Mine: return "Mine";
        case PawnAction::Cut: return "Cut";
        case PawnAction::Harvest: return "Harvest";
        case PawnAction::Build: return "Build";
        case PawnAction::Deconstruct: return "Deconstruct";
        case PawnAction::Reload: return "Reload";
        case PawnAction::Dodge: return "Dodge";
        case PawnAction::Roll: return "Roll";
        case PawnAction::Switch: return "Switch";
        case PawnAction::Extinguish: return "Extinguish";
        case PawnAction::Cook: return "Cook";
        case PawnAction::Research: return "Research";
        case PawnAction::Clean: return "Clean";
        case PawnAction::Throw: return "Throw";
        case PawnAction::Dive: return "Dive";
        case PawnAction::Handle: return "Handle";
        case PawnAction::Slaughter: return "Slaughter";
        case PawnAction::Block: return "Block";
        case PawnAction::Tend: return "Tend";
        case PawnAction::LiftPawn: return "LiftPawn";
        case PawnAction::LayPawn: return "LayPawn";
        case PawnAction::Chat: return "Chat";
        case PawnAction::FeedPawn: return "FeedPawn";
        case PawnAction::Refuel: return "Refuel";
        case PawnAction::Butcher: return "Butcher";
        case PawnAction::DropCorpse: return "DropCorpse";
        case PawnAction::Punch: return "Punch";
    }
    return "?";
}

const char* projectileHitKindName(ProjectileHitKind kind) {
    switch (kind) {
        case ProjectileHitKind::None: return "None";
        case ProjectileHitKind::Ground: return "Ground";
        case ProjectileHitKind::Rock: return "Rock";
        case ProjectileHitKind::Building: return "Building";
        case ProjectileHitKind::Pawn: return "Pawn";
        case ProjectileHitKind::Armor: return "Armor";
        case ProjectileHitKind::Gone: return "Gone";
    }
    return "?";
}

const ThingView* Snapshot::find(Entity id) const {
    const auto it = std::lower_bound(things.begin(), things.end(), id,
                                     [](const ThingView& view, Entity value) { return view.id < value; });
    return it != things.end() && it->id == id ? &*it : nullptr;
}

}  // namespace colony

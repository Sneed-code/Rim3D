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
            } else if constexpr (std::is_same_v<Type, ShortCircuitCommand>) {
                return "ShortCircuit";
            } else if constexpr (std::is_same_v<Type, SetResearchCommand>) {
                return "SetResearch";
            } else if constexpr (std::is_same_v<Type, UnlockResearchCommand>) {
                return "UnlockResearch";
            } else if constexpr (std::is_same_v<Type, StartRaidCommand>) {
                return "StartRaid";
            } else if constexpr (std::is_same_v<Type, SoundAlarmCommand>) {
                return "SoundAlarm";
            } else if constexpr (std::is_same_v<Type, SetDefenseRoleCommand>) {
                return "SetDefenseRole";
            } else if constexpr (std::is_same_v<Type, MarkShelterCommand>) {
                return "MarkShelter";
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
            } else {
                static_assert(std::is_same_v<Type, StripCommand>, "name every command type");
                return "Strip";
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
        case EventType::RaidArrived: return "RaidArrived";
        case EventType::SquadChanged: return "SquadChanged";
        case EventType::AlarmChanged: return "AlarmChanged";
        case EventType::PawnLeftMap: return "PawnLeftMap";
        case EventType::MeleeSwing: return "MeleeSwing";
        case EventType::MeleeHit: return "MeleeHit";
        case EventType::ApparelDestroyed: return "ApparelDestroyed";
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
        case PawnAction::Cook: return "Cook";
        case PawnAction::Research: return "Research";
        case PawnAction::Clean: return "Clean";
        case PawnAction::Block: return "Block";
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

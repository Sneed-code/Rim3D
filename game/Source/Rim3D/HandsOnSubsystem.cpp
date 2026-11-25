#include "HandsOnSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"
#include "Kismet/GameplayStatics.h"

#include <imgui.h>

#include "ColonyHudStyle.h"
#include "FightSubsystem.h"
#include "KeyBindings.h"
#include "OrdersSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimPicking.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Commands.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Mineables.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

DEFINE_LOG_CATEGORY_STATIC(LogHandsOn, Log, All);

namespace
{
	/** How far past the player the crosshair looks for something to work on, in cm. */
	constexpr double HandsLookCm = 1800.0;
	/** The ghost only follows the crosshair this far from the colonist, in metres. */
	constexpr double HandsBuildRangeM = 14.0;
	// F picks up a weapon lying this near (metres), as the equip key does.
	constexpr double HandsWeaponReachM = 3.0;  // the two cells of hands-on reach, diagonals too
	/** Heights of what can be looked at, in cm: a tree, a bush or a flower. */
	constexpr double HandsTreeCm = 650.0;
	constexpr double HandsBushCm = 110.0;
	constexpr double HandsMessageSeconds = 3.5;

	void HandsText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}

	FString HandsCapital(const std::string& Text)
	{
		FString Result = UTF8_TO_TCHAR(Text.c_str());
		if (!Result.IsEmpty())
		{
			Result[0] = FChar::ToUpper(Result[0]);
		}
		return Result;
	}
}

bool UHandsOnSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UHandsOnSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UHandsOnSubsystem, STATGROUP_Tickables);
}

void UHandsOnSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UHandsOnSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<UProtagonistSubsystem>();
	Collection.InitializeDependency<UOrdersSubsystem>();
}

void UHandsOnSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

void UHandsOnSubsystem::Say(const FString& Text)
{
	Message = Text;
	MessageUntil = FPlatformTime::Seconds() + HandsMessageSeconds;
	UE_LOG(LogHandsOn, Log, TEXT("%s"), *Text);
}

void UHandsOnSubsystem::OnSimEvent(const colony::Event& Event)
{
	// The sim says why it refused (too far, taken by someone else...).
	if (Event.type == colony::EventType::CommandFailed && LastCommand != 0 && Event.command == LastCommand)
	{
		Say(HandsCapital(Event.message));
		bInteracting = false;
	}
}

// ---- What is under the crosshair ---------------------------------------------------------------

FHandsOnTarget UHandsOnSubsystem::FindTarget(const USimSubsystem& Bridge, const FVector& Origin, const FVector& Direction, const FVector& Head) const
{
	FHandsOnTarget Result;
	const colony::Sim* Sim = Bridge.GetSim();
	if (!Sim)
	{
		return Result;
	}
	const colony::Map& Map = Sim->map();

	// Nothing behind the player's head counts (the third person camera hangs behind it).
	const double Start = FMath::Max(0.0, FVector::DotProduct(Head - Origin, Direction) - 30.0);
	// The ground, or an upper floor (every level is shown in direct mode).
	double GroundT = 0.0;
	int32 GroundLevel = 0;
	const bool bGround = SimPicking::RayHitGround(Bridge, Origin, Direction, GroundT, -1, colony::Map::kLevels - 1, &GroundLevel);
	const double End = bGround ? FMath::Min(GroundT, Start + HandsLookCm) : Start + HandsLookCm;
	// The ray meets a rock block at the edge of its cell: look a little past the ground hit so the march gets inside it.
	const double MarchEnd = End + 20.0;

	// The ground cell the ray ends on: somewhere to put something down.
	if (bGround)
	{
		const FVector Local = (Origin + Direction * GroundT - Bridge.GetWorldOrigin()) / 100.0;
		const colony::CellPos Cell{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), GroundLevel};
		if (Map.inBounds(Cell))
		{
			Result.Cell = Cell;
			Result.bValid = true;
		}
	}

	// Rocks and plants: march along the ray, a cell column at a time, until it is inside one.
	double BestT = TNumericLimits<double>::Max();
	colony::CellPos Found;
	for (double T = Start; T < MarchEnd; T += 10.0)
	{
		const FVector Point = Origin + Direction * T;
		const FVector Local = (Point - Bridge.GetWorldOrigin()) / 100.0;
		const colony::CellPos Cell{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), 0};
		if (!Map.inBounds(Cell))
		{
			continue;
		}
		const double GroundZ = Bridge.SimToWorld(static_cast<float>(Local.X), static_cast<float>(Local.Y), 0).Z;
		if (Sim->mineables().has(Cell) && Point.Z <= GroundZ + TerrainDraw::RockHeightM * 100.0)
		{
			BestT = T;
			Found = Cell;
			break;
		}
		if (Sim->plants().has(Cell))
		{
			const double Height = Sim->defs().get(Sim->plants().defAt(Cell)).flag("plant.isTree") ? HandsTreeCm : HandsBushCm;
			if (Point.Z <= GroundZ + Height)
			{
				BestT = T;
				Found = Cell;
				break;
			}
		}
	}

	// Items and building plans: the nearest box the ray goes through.
	colony::Entity BestThing = colony::kNoEntity;
	double BestThingT = TNumericLimits<double>::Max();
	const FVector Reach = Direction * End;
	for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
	{
		const bool bSite = View.category == colony::ThingCategory::Building && View.stage != colony::ConstructionStage::Built;
		// A finished bed: F sleeps the night away in it (docs/design/needs.md "The protagonist sleeps the night away").
		const bool bBed = View.category == colony::ThingCategory::Building && !bSite && View.def.valid() &&
			Sim->defs().get(View.def).flag("building.bed");
		FBox Box;
		if ((View.category != colony::ThingCategory::Item && !bSite && !bBed) || !SimPicking::ThingBox(Bridge, View, Box) ||
			!FMath::LineBoxIntersection(Box, Origin, Origin + Reach, Reach))
		{
			continue;
		}
		const double T = FVector::DotProduct(Box.GetCenter() - Origin, Direction);
		if (T >= Start - 150.0 && T < BestThingT)
		{
			BestThing = View.id;
			BestThingT = T;
		}
	}

	if (BestThing != colony::kNoEntity && BestThingT <= BestT)
	{
		Result.Thing = BestThing;
		Result.bGround = false;
		Result.bValid = true;
	}
	else if (BestT < TNumericLimits<double>::Max())
	{
		Result.Cell = Found;
		Result.bGround = false;
		Result.bValid = true;
	}
	return Result;
}

colony::HandsOnPlan UHandsOnSubsystem::PlanFor(const USimSubsystem& Bridge, const FHandsOnTarget& InTarget) const
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge.GetSim() || !Protagonist || !InTarget.bValid || InTarget.bGround)
	{
		return colony::HandsOnPlan();
	}
	return Bridge.GetSim()->jobs().planHandsOn(Protagonist->GetControlled(), InTarget.Thing, InTarget.Cell);
}

// ---- Keys ---------------------------------------------------------------------------------------

void UHandsOnSubsystem::SetInteractHeld(bool bHeld)
{
	bScriptedHeld = bHeld;
}

bool UHandsOnSubsystem::IsLooseWeapon(const USimSubsystem& Bridge, colony::Entity Thing, colony::Entity Pawn) const
{
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::ThingView* Item = Thing != colony::kNoEntity ? Snapshot.find(Thing) : nullptr;
	const colony::ThingView* Me = Pawn != colony::kNoEntity ? Snapshot.find(Pawn) : nullptr;
	if (!Item || !Me || Item->category != colony::ThingCategory::Item || !Item->def.valid() || !Bridge.GetDefs())
	{
		return false;
	}
	const colony::Def& Def = Bridge.GetDefs()->get(Item->def);
	return Def.text("equipmentType") == "Primary" && FVector2D::Distance(FVector2D(Me->x, Me->y), FVector2D(Item->x, Item->y)) <= HandsWeaponReachM;
}

void UHandsOnSubsystem::Interact(USimSubsystem& Bridge, bool bDown)
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::Entity Pawn = Protagonist ? Protagonist->GetControlled() : colony::kNoEntity;
	if (bDown && !bWasDown)
	{
		if (IsLooseWeapon(Bridge, Target.Thing, Pawn))
		{
			// F on a weapon lying in reach picks it up into the hands (sneed 2026-11-17: F is the one interact key). Before
			// the plain pick-up, which would only carry it like a hauler.
			LastCommand = Bridge.Submit(colony::EquipCommand{Pawn, Target.Thing});
			++Stats.CommandsSent;
		}
		else if (Plan.ok() && Plan.kind == colony::HandsOnKind::Sleep)
		{
			// A bed: a press sleeps the night away (SleepCommand); nothing is held.
			LastCommand = Bridge.Submit(colony::SleepCommand{Pawn, Target.Thing, false});
			++Stats.CommandsSent;
		}
		else if (Plan.ok())
		{
			LastCommand = Bridge.Submit(colony::HandsOnCommand{Pawn, colony::HandsOnMode::Work, Target.Thing, Target.Cell});
			++Stats.CommandsSent;
			bInteracting = Plan.kind != colony::HandsOnKind::PickUp;
			WorkKind = Plan.kind;
			bWorkSeen = false;
			WorkSentStep = Bridge.GetCurrentSnapshot().steps;
		}
		else if (Target.bValid && !Target.bGround)
		{
			Say(Plan.kind != colony::HandsOnKind::None && !Plan.inReach ? FString::Printf(TEXT("Too far to %s: get closer"), UTF8_TO_TCHAR(Plan.label.c_str()))
																		 : HandsCapital(Plan.error));
		}
	}
	else if (!bDown && bWasDown && bInteracting)
	{
		bInteracting = false;
		Bridge.Submit(colony::HandsOnCommand{Pawn, colony::HandsOnMode::Stop, colony::kNoEntity, colony::CellPos{}});
		++Stats.CommandsSent;
	}
	else if (bDown && bWasDown && bInteracting)
	{
		ContinueWork(Bridge, Pawn);
	}
	bWasDown = bDown;
}

// sneed 2026-11-25: "if I hold down F it goes to the next cell after finished (so you can rapidly cut grass etc.)". While
// F stays held and the job is over (done, or the target went), the sim names the next target of the same kind within the
// two cells' reach, the one the view points at first (JobSystem::nextHandsOn), and it goes in as an ordinary command. Not in
// a fight: locked on to an enemy or fighting, letting go of the work is the safe thing.
void UHandsOnSubsystem::ContinueWork(USimSubsystem& Bridge, colony::Entity Pawn)
{
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::ThingView* Me = Snapshot.find(Pawn);
	if (!Me)
	{
		bInteracting = false;
		return;
	}
	const bool bWorking = Me->job == colony::JobType::Mine || Me->job == colony::JobType::CutPlant || Me->job == colony::JobType::HarvestPlant ||
		Me->job == colony::JobType::BuildFrame || Me->job == colony::JobType::DeliverResources;
	if (bWorking)
	{
		bWorkSeen = true;
		return;
	}
	// Not started yet: the command waits for the next step (a paused game holds it). A few steps with no job is a refusal
	// the sim had no words for, or work that was over at once.
	if (!bWorkSeen && Snapshot.steps < WorkSentStep + 3)
	{
		return;
	}
	const bool bCellWork = WorkKind == colony::HandsOnKind::Mine || WorkKind == colony::HandsOnKind::Chop || WorkKind == colony::HandsOnKind::Cut ||
		WorkKind == colony::HandsOnKind::Harvest || WorkKind == colony::HandsOnKind::Build;
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const AProtagonistPawn* Player = Protagonist ? Protagonist->GetBody() : nullptr;
	if (!bCellWork || !Player || Me->fighting || Me->focusTarget != colony::kNoEntity || !Bridge.GetSim())
	{
		bInteracting = false;
		return;
	}
	const FVector View = Player->GetViewDirection();
	const colony::HandsOnTarget Next = Bridge.GetSim()->jobs().nextHandsOn(Pawn, WorkKind, static_cast<float>(View.X), static_cast<float>(View.Y));
	if (!Next.found)
	{
		bInteracting = false;  // nothing more of its kind in reach: holding F does nothing more until it is pressed again
		return;
	}
	LastCommand = Bridge.Submit(colony::HandsOnCommand{Pawn, colony::HandsOnMode::Work, Next.thing, Next.cell});
	++Stats.CommandsSent;
	++Stats.ChainedJobs;
	bWorkSeen = false;
	WorkSentStep = Snapshot.steps;
}

void UHandsOnSubsystem::PutDown()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return;
	}
	LastCommand = Bridge->Submit(colony::HandsOnCommand{Protagonist->GetControlled(), colony::HandsOnMode::PutDown, colony::kNoEntity, bHasAim ? AimCell : colony::CellPos{-1, -1, 0}});
	++Stats.CommandsSent;
}

bool UHandsOnSubsystem::HandleWheel(float Notches)
{
	UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	if (!Orders || !Orders->IsDirectBuilding() || FMath::IsNearlyZero(Notches))
	{
		return false;
	}
	Orders->CycleBuilding(Notches > 0.0f ? 1 : -1);
	return true;
}

void UHandsOnSubsystem::ToggleBuilding()
{
	// B (sneed 2026-11-25): the overseer's bar over this view with Build open, the mouse placing plans under the cursor.
	if (UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>())
	{
		Protagonist->ToggleBuildUi();
	}
}

bool UHandsOnSubsystem::PlaceBuilding()
{
	UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	if (!Orders || !Orders->IsDirectBuilding())
	{
		return false;
	}
	const bool bPlanned = Orders->PlaceAtHover();
	Say(bPlanned ? Orders->GetLastMessage() : FString(TEXT("Can't build there")));
	if (bPlanned)
	{
		++Stats.GhostPlans;
	}
	return bPlanned;
}

void UHandsOnSubsystem::UpdateBuilding(USimSubsystem& Bridge, APlayerController* PC)
{
	UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	if (!Orders)
	{
		return;
	}
	if (Rim3DKeys::WasPressed(PC, ERim3DAction::BuildMode))
	{
		ToggleBuilding();
	}
	if (!Orders->IsDirectBuilding())
	{
		return;
	}
	// The ghost follows the crosshair, snapped to the grid, but not to the far side of the map.
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* Me = Protagonist ? Bridge.GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	if (bHasAim && Me && FVector2D::Distance(FVector2D(AimCell.x + 0.5, AimCell.y + 0.5), FVector2D(Me->x, Me->y)) <= HandsBuildRangeM)
	{
		Orders->SetHoverCell(AimCell);
	}
	else
	{
		Orders->ClearHover();
	}
	if (!PC)
	{
		return;
	}
	if (PC->WasInputKeyJustPressed(EKeys::R))
	{
		Orders->RotateBuilding();
	}
	if (PC->WasInputKeyJustPressed(EKeys::Q))
	{
		Orders->CycleStuff();
	}
	if (PC->WasInputKeyJustPressed(EKeys::LeftMouseButton))
	{
		PlaceBuilding();
	}
	if (PC->WasInputKeyJustPressed(EKeys::RightMouseButton) || PC->WasInputKeyJustPressed(EKeys::Escape))
	{
		Orders->CancelTool();
	}
}

// ---- The tick -----------------------------------------------------------------------------------

void UHandsOnSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist)
	{
		return;
	}
	if (LastSim != Bridge->GetSim())
	{
		LastSim = Bridge->GetSim();
		Target = FHandsOnTarget();
		Plan = colony::HandsOnPlan();
		bInteracting = false;
		bWasDown = false;
		bHasAim = false;
		LastCommand = 0;
		Stats = FHandsOnStats();
	}
	AProtagonistPawn* Body = Protagonist->GetBody();
	if (!Protagonist->IsDirectMode() || !IsValid(Body))
	{
		Target = FHandsOnTarget();
		Plan = colony::HandsOnPlan();
		bInteracting = false;
		bWasDown = false;
		bHasAim = false;
		return;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);

	// What the crosshair is on, and what the colonist would do about it.
	const FVector Origin = Body->GetViewLocation();
	const FVector Direction = Body->GetViewDirection();
	FVector Head = Origin;
	FVector Feet;
	if (Bridge->GetBlendedLocation(Protagonist->GetControlled(), Feet))
	{
		Head = Feet + FVector(0.0, 0.0, 150.0);
	}
	Target = FindTarget(*Bridge, Origin, Direction, Head);
	Plan = PlanFor(*Bridge, Target);
	bHasAim = Target.bValid;
	if (Target.bValid && Target.bGround)
	{
		AimCell = Target.Cell;
	}
	else
	{
		// The ground behind what is looked at (on the player's level), for the ghost and for putting down.
		const colony::ThingView* Self = Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled());
		const FSimPick Ground = SimPicking::PickGroundFromRay(*Bridge, Origin, Direction, Self ? Self->level : 0);
		bHasAim = Ground.Kind == ESimPickKind::Cell;
		AimCell = Ground.Cell;
	}

	const bool bDown = bScripted ? bScriptedHeld : Rim3DKeys::IsDown(PC, ERim3DAction::Interact);
	Interact(*Bridge, bDown);
	if (!bScripted && Rim3DKeys::WasPressed(PC, ERim3DAction::PutDown))
	{
		PutDown();
	}
	UpdateBuilding(*Bridge, PC);
	DrawHud(*Bridge);
}

void UHandsOnSubsystem::DrawHud(USimSubsystem& Bridge)
{
	const ImGui::FScopedContext ScopedContext;
	if (!ScopedContext)
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	if (Viewport->Size.x <= 0.0f || Viewport->Size.y <= 0.0f)
	{
		return;
	}
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (Protagonist && Protagonist->IsBuildUiOpen())
	{
		return;  // the build screen (B): the overseer's panels are up, the walking prompts keep out of the way
	}
	const UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	const UFightSubsystem* Fight = GetWorld()->GetSubsystem<UFightSubsystem>();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const colony::ThingView* Me = Protagonist ? Snapshot.find(Protagonist->GetControlled()) : nullptr;
	const colony::Sim& Sim = *Bridge.GetSim();
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ColonyHud::ApplyFrameStyle(Scale);
	const ImGuiWindowFlags Flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	const ImVec2 Centre(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f);
	const float Margin = 12.0f * Scale;
	const float Gap = 6.0f * Scale;
	const double Now = FPlatformTime::Seconds();
	const FString Key = Rim3DKeys::GetKeyLabel(ERim3DAction::Interact);

	// Bottom middle, stacked from the bottom up (sneed 2026-11-25: the F box goes to the bottom middle): the weapon box
	// (UFightSubsystem), what is carried, then what F would do and the work in progress.
	const float GunTop = Fight ? Fight->GetBottomBoxTop() : 0.0f;
	float Bottom = GunTop > 0.0f ? GunTop - Gap : Viewport->Pos.y + Viewport->Size.y - Margin;

	// What is carried.
	const bool bCarrying = Me && Me->carriedCount > 0;
	if (bCarrying || (Orders && Orders->IsDirectBuilding()))
	{
		ImGui::SetNextWindowPos(ImVec2(Centre.x, Bottom), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##HandsCarry", nullptr, Flags))
		{
			if (bCarrying)
			{
				HandsText(FString::Printf(TEXT("Carrying %d %s    %s: put down    %s on a plan: put it in"), Me->carriedCount,
					UTF8_TO_TCHAR(Sim.defs().get(Me->carriedDef).label().c_str()), *Rim3DKeys::GetKeyLabel(ERim3DAction::PutDown), *Key));
			}
			if (Orders && Orders->IsDirectBuilding())
			{
				HandsText(FString::Printf(TEXT("Building: %s"), *Orders->GetBuildSummary()));
				ImGui::TextUnformatted("wheel: building   Q: stuff   R: turn   click: plan it   B: put away");
			}
			Bottom = ImGui::GetWindowPos().y - Gap;
		}
		ImGui::End();
	}
	CarryTop = Bottom;

	// What F would do, or why not, with the work's progress under it. An item says it on itself instead (DrawWorldLabel).
	const colony::ThingView* Looked = Target.bValid && !Target.bGround && Target.Thing != colony::kNoEntity ? Snapshot.find(Target.Thing) : nullptr;
	const bool bItem = Looked && Looked->category == colony::ThingCategory::Item;
	const bool bPrompt = Target.bValid && !Target.bGround && !bItem && (Plan.kind != colony::HandsOnKind::None || !Plan.error.empty());
	const bool bWorking = Me && (Me->job == colony::JobType::Mine || Me->job == colony::JobType::CutPlant ||
		Me->job == colony::JobType::HarvestPlant || Me->job == colony::JobType::BuildFrame);
	if (bPrompt || bWorking)
	{
		ImGui::SetNextWindowPos(ImVec2(Centre.x, Bottom), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##HandsPrompt", nullptr, Flags))
		{
			if (bPrompt)
			{
				const FString Label = HandsCapital(Plan.label);
				if (Plan.kind == colony::HandsOnKind::None)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));
					HandsText(HandsCapital(Plan.error));
				}
				else if (Plan.ok())
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.95f, 0.55f, 1.0f));
					HandsText(Plan.kind == colony::HandsOnKind::Sleep ? FString::Printf(TEXT("%s   %s"), *Key, *Label)
																	 : FString::Printf(TEXT("%s   hold: %s"), *Key, *Label));
				}
				else
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.8f, 0.6f, 1.0f));
					HandsText(Plan.inReach ? FString::Printf(TEXT("%s: %s"), *Label, *HandsCapital(Plan.error)) : FString::Printf(TEXT("%s: get closer"), *Label));
				}
				ImGui::PopStyleColor();
				++Stats.PromptFrames;
			}
			if (bWorking)
			{
				const char* Verb = Me->job == colony::JobType::Mine ? "Mining" : (Me->job == colony::JobType::BuildFrame ? "Building" : "Working");
				ImGui::TextUnformatted(Verb);
				ImGui::ProgressBar(FMath::Clamp(Me->jobProgress, 0.0f, 1.0f), ImVec2(220.0f * Scale, 14.0f * Scale), "");
				++Stats.ProgressFrames;
			}
		}
		ImGui::End();
	}

	DrawWorldLabel(Bridge, Scale);

	// The last thing the sim or the ghost said.
	if (!Message.IsEmpty() && Now < MessageUntil)
	{
		ImGui::SetNextWindowPos(ImVec2(Centre.x, Centre.y - 90.0f * Scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		ImGui::SetNextWindowBgAlpha(0.6f);
		if (ImGui::Begin("##HandsMessage", nullptr, Flags))
		{
			HandsText(Message);
		}
		ImGui::End();
	}

	++Stats.FramesDrawn;
}

// sneed 2026-11-25: "If there's something to pick up, when I hover over it with my view it has the text on the object
// instead." The key and what it does, over the item in the world: green when it can be done, else why not.
void UHandsOnSubsystem::DrawWorldLabel(USimSubsystem& Bridge, float Scale)
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* Item = Target.bValid && !Target.bGround && Target.Thing != colony::kNoEntity ? Bridge.GetCurrentSnapshot().find(Target.Thing) : nullptr;
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	FBox Box;
	if (!Protagonist || !Item || !PC || !Bridge.GetSim() || Item->category != colony::ThingCategory::Item || !Item->def.valid() ||
		!SimPicking::ThingBox(Bridge, *Item, Box))
	{
		return;
	}
	FString Name = HandsCapital(Bridge.GetSim()->defs().get(Item->def).label());
	if (Item->stackCount > 1)
	{
		Name = FString::Printf(TEXT("%s x%d"), *Name, Item->stackCount);
	}
	const FString Key = Rim3DKeys::GetKeyLabel(ERim3DAction::Interact);
	FString Text;
	bool bCan = false;
	if (IsLooseWeapon(Bridge, Target.Thing, Protagonist->GetControlled()) || (Plan.kind == colony::HandsOnKind::PickUp && Plan.ok()))
	{
		Text = FString::Printf(TEXT("%s   Pick up %s"), *Key, *Name);
		bCan = true;
	}
	else if (Plan.kind == colony::HandsOnKind::PickUp && !Plan.inReach)
	{
		Text = FString::Printf(TEXT("%s: get closer"), *Name);
	}
	else
	{
		Text = FString::Printf(TEXT("%s: %s"), *Name, Plan.error.empty() ? TEXT("Can't take that") : *HandsCapital(Plan.error));
	}
	// Just over the top of it, in the middle.
	FVector2D Screen;
	const FVector Over(Box.GetCenter().X, Box.GetCenter().Y, Box.Max.Z + 25.0);
	if (!PC->ProjectWorldLocationToScreen(Over, Screen, true))
	{
		return;
	}
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const FTCHARToUTF8 Utf8(*Text);
	const ImVec2 Size = ImGui::CalcTextSize(Utf8.Get(), Utf8.Get() + Utf8.Length());
	const float Pad = 6.0f * Scale;
	const ImVec2 At(Viewport->Pos.x + static_cast<float>(Screen.X) - Size.x * 0.5f, Viewport->Pos.y + static_cast<float>(Screen.Y) - Size.y - Pad);
	ImDrawList* Draw = ImGui::GetForegroundDrawList();
	Draw->AddRectFilled(ImVec2(At.x - Pad, At.y - Pad * 0.6f), ImVec2(At.x + Size.x + Pad, At.y + Size.y + Pad * 0.6f), IM_COL32(10, 12, 14, 165), 4.0f * Scale);
	Draw->AddText(At, bCan ? IM_COL32(140, 242, 140, 255) : IM_COL32(220, 210, 170, 255), Utf8.Get(), Utf8.Get() + Utf8.Length());
	++Stats.WorldLabelFrames;
}

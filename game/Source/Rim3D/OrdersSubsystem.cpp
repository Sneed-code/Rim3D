#include "OrdersSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include <imgui.h>

#include "BuildingSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "VisualMesh.h"

#include "ColonySim/Construction.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Sim.h"

#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogOrders, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GOrdersMarks(
		TEXT("Orders.Marks"), 1, TEXT("1 draws the marks of the orders (mine, cut, harvest, deconstruct, stockpile cells), 0 hides them."), ECVF_Default);

	// The same flat-colour material as the terrain and buildings, and the see-through one the BuildingMaterials commandlet makes.
	const TCHAR* const OrdSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const OrdGhostMaterialPath = TEXT("/Game/Rim3D/Generated/Buildings/M_BuildingGhost.M_BuildingGhost");

	// What an overlay instance is. The key of a component is the kind times 10000 plus the post height in 50 cm steps.
	constexpr int32 OrdMine = 1;
	constexpr int32 OrdCut = 2;
	constexpr int32 OrdHarvest = 3;
	constexpr int32 OrdDeconstruct = 4;
	constexpr int32 OrdZoneTile = 5;
	constexpr int32 OrdZoneRing = 6;
	constexpr int32 OrdKeyScale = 10000;

	constexpr int32 OrdMaxBuildOrigins = 200;
	constexpr int32 OrdPostMineCm = 150;
	constexpr double OrdMessageSeconds = 4.0;

	const FLinearColor OrdMineColor = FLinearColor::FromSRGBColor(FColor(255, 140, 30));
	const FLinearColor OrdCutColor = FLinearColor::FromSRGBColor(FColor(70, 210, 70));
	const FLinearColor OrdHarvestColor = FLinearColor::FromSRGBColor(FColor(245, 215, 50));
	const FLinearColor OrdDeconstructColor = FLinearColor::FromSRGBColor(FColor(225, 50, 50));
	const FLinearColor OrdZoneColor = FLinearColor::FromSRGBColor(FColor(60, 150, 230));

	uint64 OrdMix(uint64 Seed, uint64 Value)
	{
		Seed ^= Value + 0x9E3779B97F4A7C15ULL + (Seed << 6) + (Seed >> 2);
		return Seed;
	}

	uint64 OrdCellKey(int32 X, int32 Y)
	{
		return (static_cast<uint64>(static_cast<uint32>(X)) << 32) | static_cast<uint32>(Y);
	}

	colony::CellRect OrdRectOf(const colony::CellPos A, const colony::CellPos B)
	{
		const int32 MinX = FMath::Min(A.x, B.x);
		const int32 MinY = FMath::Min(A.y, B.y);
		return colony::CellRect{MinX, MinY, FMath::Abs(A.x - B.x) + 1, FMath::Abs(A.y - B.y) + 1, 0};
	}

	/** The cells a building, plan or frame covers, from its snapshot entry. */
	colony::CellRect OrdThingRect(const colony::ThingView& View)
	{
		return colony::CellRect{FMath::RoundToInt32(View.x - static_cast<float>(View.sizeX) * 0.5f), FMath::RoundToInt32(View.y - static_cast<float>(View.sizeY) * 0.5f),
			FMath::Max(1, View.sizeX), FMath::Max(1, View.sizeY), 0};
	}

	bool OrdOverlap(const colony::CellRect& A, const colony::CellRect& B)
	{
		return A.minX < B.minX + B.sizeX && B.minX < A.minX + A.sizeX && A.minY < B.minY + B.sizeY && B.minY < A.minY + A.sizeY;
	}

	/** A corner of a cell on the surface that is drawn (the rock block included), lifted a little so lines don't sink into the ground. */
	FVector OrdCorner(const USimSubsystem& Bridge, int32 X, int32 Y)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		FVector Point = Bridge.SimToWorld(FMath::Clamp(static_cast<float>(X), 0.0f, static_cast<float>(Map.width()) - 0.01f),
			FMath::Clamp(static_cast<float>(Y), 0.0f, static_cast<float>(Map.height()) - 0.01f), 0);
		double Bonus = 0.0;
		for (int32 DY = -1; DY <= 0; ++DY)
		{
			for (int32 DX = -1; DX <= 0; ++DX)
			{
				Bonus = FMath::Max(Bonus, TerrainDraw::SurfaceBonusCm(Map, X + DX + 0.5, Y + DY + 0.5));
			}
		}
		Point.Z += Bonus + 8.0;
		return Point;
	}

	/** The top of a cell's drawn surface (the highest corner, the rock block included), in world units. */
	double OrdCellTopCm(const USimSubsystem& Bridge, int32 X, int32 Y)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		float High = -1.0e9f;
		for (int32 DY = 0; DY <= 1; ++DY)
		{
			for (int32 DX = 0; DX <= 1; ++DX)
			{
				High = FMath::Max(High, Map.groundHeight(FMath::Clamp(X + DX, 0, Map.width() - 1), FMath::Clamp(Y + DY, 0, Map.height() - 1)));
			}
		}
		return Bridge.GetWorldOrigin().Z + static_cast<double>(High) * 100.0 + TerrainDraw::SurfaceBonusCm(Map, X + 0.5, Y + 0.5);
	}

	void OrdDrawRect(UWorld* World, const USimSubsystem& Bridge, const colony::CellRect& Rect, const FColor& Color, float Thickness)
	{
		const auto Segment = [&](int32 X0, int32 Y0, int32 X1, int32 Y1)
		{
			DrawDebugLine(World, OrdCorner(Bridge, X0, Y0), OrdCorner(Bridge, X1, Y1), Color, false, -1.0f, SDPG_Foreground, Thickness);
		};
		for (int32 X = Rect.minX; X < Rect.minX + Rect.sizeX; ++X)
		{
			Segment(X, Rect.minY, X + 1, Rect.minY);
			Segment(X, Rect.minY + Rect.sizeY, X + 1, Rect.minY + Rect.sizeY);
		}
		for (int32 Y = Rect.minY; Y < Rect.minY + Rect.sizeY; ++Y)
		{
			Segment(Rect.minX, Y, Rect.minX, Y + 1);
			Segment(Rect.minX + Rect.sizeX, Y, Rect.minX + Rect.sizeX, Y + 1);
		}
	}

	FColor OrdToolColor(EOrderTool Tool)
	{
		switch (Tool)
		{
			case EOrderTool::Mine: return FColor(255, 140, 30);
			case EOrderTool::Chop: return FColor(70, 210, 70);
			case EOrderTool::Harvest: return FColor(245, 215, 50);
			case EOrderTool::Stockpile: return FColor(60, 150, 230);
			case EOrderTool::RemoveZone: return FColor(160, 160, 170);
			case EOrderTool::Cancel: return FColor(255, 255, 255);
			case EOrderTool::Deconstruct: return FColor(225, 50, 50);
			default: return FColor(255, 255, 255);
		}
	}

	FString OrdCapital(const std::string& Text)
	{
		FString Out = UTF8_TO_TCHAR(Text.c_str());
		if (!Out.IsEmpty())
		{
			Out[0] = FChar::ToUpper(Out[0]);
		}
		return Out;
	}

	bool OrdButton(const FString& Label, bool bActive)
	{
		if (bActive)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.75f, 1.0f));
		}
		const bool bClicked = ImGui::Button(TCHAR_TO_UTF8(*Label));
		if (bActive)
		{
			ImGui::PopStyleColor();
		}
		return bClicked;
	}

	void OrdText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

bool UOrdersSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UOrdersSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOrdersSubsystem, STATGROUP_Tickables);
}

void UOrdersSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
}

void UOrdersSubsystem::Deinitialize()
{
	OverlayActor = nullptr;
	OverlayComponents.Reset();
	GhostGood = nullptr;
	GhostBad = nullptr;
	Super::Deinitialize();
}

USimSubsystem* UOrdersSubsystem::GetBridge() const
{
	UWorld* World = GetWorld();
	return World ? World->GetSubsystem<USimSubsystem>() : nullptr;
}

bool UOrdersSubsystem::IsOverseerActive() const
{
	UWorld* World = GetWorld();
	const UOverseerSubsystem* Overseer = World ? World->GetSubsystem<UOverseerSubsystem>() : nullptr;
	return Overseer && Overseer->IsOverseerActive();
}

void UOrdersSubsystem::Say(const FString& Text)
{
	Message = Text;
	MessageUntil = FPlatformTime::Seconds() + OrdMessageSeconds;
	UE_LOG(LogOrders, Log, TEXT("%s"), *Text);
}

void UOrdersSubsystem::Submit(colony::Command Command)
{
	if (USimSubsystem* Bridge = GetBridge())
	{
		Bridge->Submit(std::move(Command));
		++CommandsSent;
	}
}

void UOrdersSubsystem::FlushIfPaused()
{
	// Commands are applied by the next step; while the game is paused, one step makes the order show at once.
	if (USimSubsystem* Bridge = GetBridge(); Bridge && Bridge->GetSpeed() == 0)
	{
		Bridge->StepOnce();
	}
}

// ---- The sim changed ---------------------------------------------------------------------------

void UOrdersSubsystem::ResetForNewSim()
{
	Tool = EOrderTool::None;
	bDragging = false;
	bHasHover = false;
	BuildDef = colony::DefId();
	BuildStuff = colony::DefId();
	BuildRot = colony::Rot4::North;
	OpenCategory.Reset();
	StuffOptions.Reset();
	StuffOptionsSeconds = -1;
	CommandsSent = 0;
	Message.Reset();
	// The overlay was made for the old sim's origin; start it over.
	if (OverlayActor)
	{
		OverlayActor->Destroy();
	}
	OverlayActor = nullptr;
	OverlayComponents.Reset();
	MarkMeshes.Reset();
	KeepMeshes.Reset();
	GhostGood = nullptr;
	GhostBad = nullptr;
	ZoneMaterial = nullptr;
	GhostGoodMaterial = nullptr;
	GhostBadMaterial = nullptr;
	MarksSignature = 0;
	PreviewSignature = 0;
	OverlaySeconds = -1;
	Stats = FOrdersStats();
	ReadBuildOptions();
}

void UOrdersSubsystem::ReadBuildOptions()
{
	BuildOptions.Reset();
	BuildCategories.Reset();
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::DefDatabase& Defs = Sim.defs();
	for (const colony::DefId Id : Defs.idsOfKind("Things"))
	{
		const colony::Def& Def = Defs.get(Id);
		if (Def.text("category") != "Building" || !Def.has("designationCategory") || !Sim.construction().buildable(Id))
		{
			continue;
		}
		FBuildOption Option;
		Option.Def = Id;
		Option.Label = OrdCapital(Def.label());
		Option.Category = OrdCapital(Def.text("designationCategory"));
		BuildOptions.Add(Option);
		BuildCategories.AddUnique(Option.Category);
	}
	// The usual order first, anything else after it.
	const TCHAR* const Usual[] = {TEXT("Structure"), TEXT("Furniture"), TEXT("Production")};
	BuildCategories.Sort([&Usual](const FString& A, const FString& B)
	{
		const auto Rank = [&Usual](const FString& Name)
		{
			for (int32 Index = 0; Index < 3; ++Index)
			{
				if (Name == Usual[Index])
				{
					return Index;
				}
			}
			return 3;
		};
		const int32 RankA = Rank(A);
		const int32 RankB = Rank(B);
		return RankA != RankB ? RankA < RankB : A < B;
	});
}

void UOrdersSubsystem::RefreshStuffOptions()
{
	StuffOptions.Reset();
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !BuildDef.valid())
	{
		return;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::DefDatabase& Defs = Sim.defs();
	StuffOptionsSeconds = Bridge->GetCurrentSnapshot().gameSeconds;
	for (const colony::DefId Id : Defs.idsOfKind("Things"))
	{
		const colony::Def& Def = Defs.get(Id);
		if (Def.text("category") != "Item" || Def.count("stuffProps.categories") == 0 || !Sim.construction().stuffFits(BuildDef, Id))
		{
			continue;
		}
		FStuffOption Option;
		Option.Def = Id;
		Option.Label = OrdCapital(Def.label());
		StuffOptions.Add(Option);
	}
	if (StuffOptions.IsEmpty())
	{
		return;
	}
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Item)
		{
			continue;
		}
		for (FStuffOption& Option : StuffOptions)
		{
			if (Option.Def == View.def)
			{
				Option.InStock += View.stackCount;
			}
		}
	}
}

colony::DefId UOrdersSubsystem::BestStuffFor(colony::DefId Def) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Def.valid() || Bridge->GetSim()->construction().stuffFits(Def, colony::DefId()))
	{
		return colony::DefId();
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	colony::DefId Best;
	int32 BestStock = -1;
	for (const colony::DefId Id : Sim.defs().idsOfKind("Things"))
	{
		if (Sim.defs().get(Id).text("category") != "Item" || !Sim.construction().stuffFits(Def, Id))
		{
			continue;
		}
		int32 Stock = 0;
		for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
		{
			Stock += View.category == colony::ThingCategory::Item && View.def == Id ? View.stackCount : 0;
		}
		if (Stock > BestStock)
		{
			Best = Id;
			BestStock = Stock;
		}
	}
	return Best;
}

// ---- Tools -------------------------------------------------------------------------------------

void UOrdersSubsystem::SetTool(EOrderTool NewTool)
{
	bDragging = false;
	Tool = NewTool;
	if (Tool == EOrderTool::Build && !BuildDef.valid() && !BuildOptions.IsEmpty())
	{
		BuildDef = BuildOptions[0].Def;
		BuildStuff = BestStuffFor(BuildDef);
		RefreshStuffOptions();
	}
}

bool UOrdersSubsystem::SelectBuilding(const FString& DefName, const FString& Stuff)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return false;
	}
	const colony::DefDatabase& Defs = Bridge->GetSim()->defs();
	const colony::DefId Id = Defs.idOf("Things", TCHAR_TO_UTF8(*DefName));
	if (!Id.valid() || !BuildOptions.ContainsByPredicate([Id](const FBuildOption& Option) { return Option.Def == Id; }))
	{
		return false;
	}
	colony::DefId StuffId = Stuff.IsEmpty() ? BestStuffFor(Id) : Defs.idOf("Things", TCHAR_TO_UTF8(*Stuff));
	if (!Bridge->GetSim()->construction().stuffFits(Id, StuffId))
	{
		StuffId = BestStuffFor(Id);
	}
	bDragging = false;
	Tool = EOrderTool::Build;
	BuildDef = Id;
	BuildStuff = StuffId;
	BuildRot = colony::Rot4::North;
	RefreshStuffOptions();
	return true;
}

void UOrdersSubsystem::RotateBuilding()
{
	BuildRot = static_cast<colony::Rot4>((static_cast<uint8>(BuildRot) + 1) % 4);
}

bool UOrdersSubsystem::CancelTool()
{
	const bool bHadOne = Tool != EOrderTool::None || bDragging;
	Tool = EOrderTool::None;
	bDragging = false;
	return bHadOne;
}

// ---- Walking the colony ------------------------------------------------------------------------

bool UOrdersSubsystem::IsDirectBuilding() const
{
	const UWorld* World = GetWorld();
	const UProtagonistSubsystem* Protagonist = World ? World->GetSubsystem<UProtagonistSubsystem>() : nullptr;
	return Tool == EOrderTool::Build && Protagonist && Protagonist->IsDirectMode();
}

bool UOrdersSubsystem::CycleBuilding(int32 Direction)
{
	if (BuildOptions.IsEmpty())
	{
		return false;
	}
	int32 Index = BuildOptions.IndexOfByPredicate([this](const FBuildOption& Option) { return Option.Def == BuildDef; });
	Index = Index < 0 ? (Direction >= 0 ? 0 : BuildOptions.Num() - 1) : (Index + (Direction >= 0 ? 1 : -1) + BuildOptions.Num()) % BuildOptions.Num();
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return false;
	}
	return SelectBuilding(UTF8_TO_TCHAR(Bridge->GetSim()->defs().get(BuildOptions[Index].Def).defName().c_str()));
}

bool UOrdersSubsystem::CycleStuff()
{
	if (Tool != EOrderTool::Build || !BuildDef.valid())
	{
		return false;
	}
	RefreshStuffOptions();
	if (StuffOptions.Num() < 2)
	{
		return false;
	}
	const int32 Index = StuffOptions.IndexOfByPredicate([this](const FStuffOption& Option) { return Option.Def == BuildStuff; });
	BuildStuff = StuffOptions[(Index + 1 + StuffOptions.Num()) % StuffOptions.Num()].Def;
	return true;
}

FString UOrdersSubsystem::GetBuildSummary() const
{
	const USimSubsystem* Bridge = GetBridge();
	if (Tool != EOrderTool::Build || !BuildDef.valid() || !Bridge || !Bridge->GetSim())
	{
		return FString();
	}
	const colony::DefDatabase& Defs = Bridge->GetSim()->defs();
	FString Text = OrdCapital(Defs.get(BuildDef).label());
	if (BuildStuff.valid())
	{
		Text += FString::Printf(TEXT(" (%s)"), UTF8_TO_TCHAR(Defs.get(BuildStuff).label().c_str()));
	}
	return Text;
}

bool UOrdersSubsystem::PlaceAtHover()
{
	if (Tool != EOrderTool::Build || !bHasHover)
	{
		return false;
	}
	const int32 SentBefore = CommandsSent;
	BeginDrag(Hover);
	EndDrag(Hover);
	return CommandsSent > SentBefore;
}

// ---- Mouse and cells ---------------------------------------------------------------------------

FSimPick UOrdersSubsystem::PickGroundAt(const FVector2D& Screen) const
{
	UWorld* World = GetWorld();
	const USimSubsystem* Bridge = GetBridge();
	const APlayerController* PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
	FVector Origin;
	FVector Direction;
	if (!Bridge || !PC || !PC->DeprojectScreenPositionToWorld(static_cast<float>(Screen.X), static_cast<float>(Screen.Y), Origin, Direction))
	{
		return FSimPick();
	}
	return SimPicking::PickGroundFromRay(*Bridge, Origin, Direction);
}

bool UOrdersSubsystem::HandleLeftPressed(const FVector2D& Screen)
{
	if (Tool == EOrderTool::None || !IsOverseerActive())
	{
		return false;
	}
	const FSimPick Pick = PickGroundAt(Screen);
	if (Pick.Kind == ESimPickKind::Cell)
	{
		BeginDrag(Pick.Cell);
	}
	return true;
}

bool UOrdersSubsystem::HandleLeftReleased(const FVector2D& Screen)
{
	if (!bDragging)
	{
		return false;
	}
	const FSimPick Pick = PickGroundAt(Screen);
	EndDrag(Pick.Kind == ESimPickKind::Cell ? Pick.Cell : Hover);
	return true;
}

bool UOrdersSubsystem::HandleRightClick(const FVector2D& Screen)
{
	if (Tool != EOrderTool::None || bDragging)
	{
		CancelTool();
		return true;
	}
	USimSubsystem* Bridge = GetBridge();
	UOverseerSubsystem* Overseer = GetWorld() ? GetWorld()->GetSubsystem<UOverseerSubsystem>() : nullptr;
	AOverseerPawn* Camera = Overseer ? Overseer->GetOverseerPawn() : nullptr;
	const colony::ThingView* Selected = Bridge ? SelectedColonist(*Bridge) : nullptr;
	if (!Selected || !Selected->drafted || !Camera)
	{
		return false;
	}
	const FSimPick Pick = Camera->PickAtScreenPosition(Screen);
	if (Pick.Kind == ESimPickKind::Thing)
	{
		const colony::ThingView* Target = Bridge->GetCurrentSnapshot().find(Pick.Thing);
		if (Target && Target->category == colony::ThingCategory::Pawn && Target->faction != Selected->faction && !Target->dead)
		{
			return OrderAttackSelected(Target->id);
		}
	}
	if (Pick.Kind == ESimPickKind::None)
	{
		return false;
	}
	const FVector Local = (Pick.Location - Bridge->GetWorldOrigin()) / 100.0;
	const colony::CellPos Cell{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), 0};
	return OrderMoveSelected(Cell);
}

bool UOrdersSubsystem::HandleRotateOrDraftKey()
{
	if (Tool == EOrderTool::Build)
	{
		RotateBuilding();
		return true;
	}
	return ToggleDraftSelected();
}

void UOrdersSubsystem::SetHoverCell(colony::CellPos Cell)
{
	Hover = Cell;
	bHasHover = true;
}

void UOrdersSubsystem::BeginDrag(colony::CellPos Cell)
{
	if (Tool == EOrderTool::None)
	{
		return;
	}
	SetHoverCell(Cell);
	DragStart = Cell;
	bDragging = true;
}

colony::CellRect UOrdersSubsystem::DragRect() const
{
	return OrdRectOf(bDragging ? DragStart : Hover, Hover);
}

void UOrdersSubsystem::GetBuildOrigins(TArray<colony::CellPos>& Out) const
{
	Out.Reset();
	const USimSubsystem* Bridge = GetBridge();
	if (Tool != EOrderTool::Build || !BuildDef.valid() || !bHasHover || !Bridge || !Bridge->GetSim())
	{
		return;
	}
	const colony::Def& Def = Bridge->GetSim()->defs().get(BuildDef);
	const int32 SizeX = FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Def.numberAt("size", 0, 1.0))));
	const int32 SizeY = FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Def.numberAt("size", 1, 1.0))));
	const colony::CellRect Foot = colony::footprintRect(colony::CellPos{0, 0, 0}, SizeX, SizeY, BuildRot);
	if (Foot.sizeX == 1 && Foot.sizeY == 1)
	{
		// A 1 x 1 building is dragged out along a line (the longer way).
		const colony::CellPos From = bDragging ? DragStart : Hover;
		const int32 DX = Hover.x - From.x;
		const int32 DY = Hover.y - From.y;
		const bool bAlongX = FMath::Abs(DX) >= FMath::Abs(DY);
		const int32 Length = FMath::Min(OrdMaxBuildOrigins, (bAlongX ? FMath::Abs(DX) : FMath::Abs(DY)) + 1);
		const int32 Step = (bAlongX ? DX : DY) < 0 ? -1 : 1;
		for (int32 Index = 0; Index < Length; ++Index)
		{
			Out.Add(bAlongX ? colony::CellPos{From.x + Index * Step, From.y, 0} : colony::CellPos{From.x, From.y + Index * Step, 0});
		}
	}
	else
	{
		// The footprint is centred on the cursor's cell.
		Out.Add(colony::CellPos{Hover.x - (Foot.sizeX - 1) / 2, Hover.y - (Foot.sizeY - 1) / 2, 0});
	}
}

bool UOrdersSubsystem::CanPlace(colony::DefId Def, colony::CellPos Origin, colony::Rot4 Rot) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Def.valid())
	{
		return false;
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::Def& Data = Sim.defs().get(Def);
	const colony::CellRect Rect = colony::footprintRect(Origin, FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Data.numberAt("size", 0, 1.0)))),
		FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Data.numberAt("size", 1, 1.0)))), Rot);
	for (int32 Y = Rect.minY; Y < Rect.minY + Rect.sizeY; ++Y)
	{
		for (int32 X = Rect.minX; X < Rect.minX + Rect.sizeX; ++X)
		{
			const colony::CellPos Cell{X, Y, 0};
			if (!Sim.map().inBounds(Cell) || !Sim.map().walkable(Cell) || Sim.map().buildingAt(Cell) != colony::kNoEntity ||
				Sim.construction().siteAt(Cell) != colony::kNoEntity)
			{
				return false;
			}
		}
	}
	return true;
}

int32 UOrdersSubsystem::EndDrag(colony::CellPos Cell)
{
	USimSubsystem* Bridge = GetBridge();
	if (!bDragging || !Bridge || !Bridge->GetSim())
	{
		bDragging = false;
		return 0;
	}
	SetHoverCell(Cell);
	const int32 SentBefore = CommandsSent;
	const colony::CellRect Rect = DragRect();
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	switch (Tool)
	{
		case EOrderTool::Build:
		{
			TArray<colony::CellPos> Origins;
			GetBuildOrigins(Origins);
			const std::string DefName = Bridge->GetSim()->defs().get(BuildDef).defName();
			const std::string StuffName = BuildStuff.valid() ? Bridge->GetSim()->defs().get(BuildStuff).defName() : std::string();
			int32 Skipped = 0;
			for (const colony::CellPos& Origin : Origins)
			{
				if (!CanPlace(BuildDef, Origin, BuildRot))
				{
					++Skipped;
					continue;
				}
				Submit(colony::PlaceBlueprintCommand{DefName, StuffName, Origin, BuildRot});
			}
			const int32 Planned = CommandsSent - SentBefore;
			Say(Planned > 0 ? FString::Printf(TEXT("Planned %d x %s%s"), Planned, *OrdCapital(Bridge->GetSim()->defs().get(BuildDef).label()),
					Skipped > 0 ? *FString::Printf(TEXT(" (%d cells blocked)"), Skipped) : TEXT(""))
				: FString(TEXT("Can't build there")));
			break;
		}
		case EOrderTool::Stockpile:
			Submit(colony::CreateStockpileCommand{Rect, colony::StoragePriority::Normal});
			Say(FString::Printf(TEXT("Stockpile zone %d x %d"), Rect.sizeX, Rect.sizeY));
			break;
		case EOrderTool::RemoveZone:
			Submit(colony::RemoveZoneCellsCommand{Rect});
			Say(TEXT("Zone cells removed"));
			break;
		case EOrderTool::Mine:
			Submit(colony::DesignateCommand{Rect, colony::DesignationKind::Mine});
			Say(FString::Printf(TEXT("Mine %d x %d"), Rect.sizeX, Rect.sizeY));
			break;
		case EOrderTool::Chop:
			Submit(colony::DesignateCommand{Rect, colony::DesignationKind::CutPlant});
			Say(FString::Printf(TEXT("Chop %d x %d"), Rect.sizeX, Rect.sizeY));
			break;
		case EOrderTool::Harvest:
			Submit(colony::DesignateCommand{Rect, colony::DesignationKind::HarvestPlant});
			Say(FString::Printf(TEXT("Harvest %d x %d"), Rect.sizeX, Rect.sizeY));
			break;
		case EOrderTool::Cancel:
		{
			bool bMark = false;
			for (const colony::DesignationView& Mark : Snapshot.designations)
			{
				bMark = bMark || Rect.contains(Mark.cell);
			}
			if (bMark)
			{
				Submit(colony::CancelDesignationsCommand{Rect, colony::DesignationKind::None});
			}
			for (const colony::ThingView& View : Snapshot.things)
			{
				if (View.category == colony::ThingCategory::Building && (View.stage != colony::ConstructionStage::Built || View.markedForDeconstruct) &&
					OrdOverlap(Rect, OrdThingRect(View)))
				{
					Submit(colony::CancelBuildCommand{View.id});
				}
			}
			Say(CommandsSent > SentBefore ? FString(TEXT("Cancelled")) : FString(TEXT("Nothing to cancel there")));
			break;
		}
		case EOrderTool::Deconstruct:
			for (const colony::ThingView& View : Snapshot.things)
			{
				if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && !View.markedForDeconstruct &&
					OrdOverlap(Rect, OrdThingRect(View)))
				{
					Submit(colony::DeconstructCommand{View.id});
				}
			}
			Say(CommandsSent > SentBefore ? FString(TEXT("Marked to take down")) : FString(TEXT("No building there")));
			break;
		default:
			break;
	}
	bDragging = false;
	if (CommandsSent > SentBefore)
	{
		FlushIfPaused();
	}
	return CommandsSent - SentBefore;
}

// ---- Fighting ----------------------------------------------------------------------------------

const colony::ThingView* UOrdersSubsystem::SelectedColonist(const USimSubsystem& Bridge) const
{
	const UOverseerSubsystem* Overseer = GetWorld() ? GetWorld()->GetSubsystem<UOverseerSubsystem>() : nullptr;
	if (!Overseer || Overseer->GetSelection().Kind != ESimPickKind::Thing || !Bridge.GetSim())
	{
		return nullptr;
	}
	const colony::ThingView* View = Bridge.GetCurrentSnapshot().find(Overseer->GetSelection().Thing);
	return View && View->category == colony::ThingCategory::Pawn && View->faction == 1 && !View->dead && Bridge.GetSim()->colonistOf(View->id) ? View : nullptr;
}

bool UOrdersSubsystem::SetDraft(colony::Entity Pawn, bool bDraft)
{
	USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? Bridge->GetCurrentSnapshot().find(Pawn) : nullptr;
	if (!View || View->drafted == bDraft)
	{
		return false;
	}
	Submit(colony::DraftCommand{Pawn, bDraft});
	return true;
}

bool UOrdersSubsystem::DraftSelected(bool bDraft)
{
	USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? SelectedColonist(*Bridge) : nullptr;
	if (!View || !SetDraft(View->id, bDraft))
	{
		return false;
	}
	FlushIfPaused();
	return true;
}

bool UOrdersSubsystem::ToggleDraftSelected()
{
	USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? SelectedColonist(*Bridge) : nullptr;
	return View && DraftSelected(!View->drafted);
}

int32 UOrdersSubsystem::Alarm()
{
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return 0;
	}
	// Until the raid task (F6) brings the defence plans, the alarm calls every colonist to arms.
	TArray<colony::Entity> Colonists;
	bool bAnyUndrafted = false;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed && Bridge->GetSim()->colonistOf(View.id))
		{
			Colonists.Add(View.id);
			bAnyUndrafted = bAnyUndrafted || !View.drafted;
		}
	}
	int32 Changed = 0;
	for (const colony::Entity Pawn : Colonists)
	{
		Changed += SetDraft(Pawn, bAnyUndrafted) ? 1 : 0;
	}
	if (Changed > 0)
	{
		Say(bAnyUndrafted ? FString::Printf(TEXT("Alarm: %d colonists drafted"), Changed) : FString::Printf(TEXT("Stand down: %d colonists back to work"), Changed));
		FlushIfPaused();
	}
	return Changed;
}

bool UOrdersSubsystem::OrderMoveSelected(colony::CellPos Cell)
{
	USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? SelectedColonist(*Bridge) : nullptr;
	if (!View || !View->drafted)
	{
		return false;
	}
	Submit(colony::MoveOrderCommand{View->id, Cell});
	FlushIfPaused();
	return true;
}

bool UOrdersSubsystem::OrderAttackSelected(colony::Entity Target)
{
	USimSubsystem* Bridge = GetBridge();
	const colony::ThingView* View = Bridge ? SelectedColonist(*Bridge) : nullptr;
	if (!View || !View->drafted)
	{
		return false;
	}
	Submit(colony::AttackOrderCommand{View->id, Target});
	FlushIfPaused();
	return true;
}

void UOrdersSubsystem::DrawDraftControls(const colony::ThingView& View)
{
	if (View.category != colony::ThingCategory::Pawn || View.faction != 1 || View.dead)
	{
		return;
	}
	if (View.drafted)
	{
		if (ImGui::Button("Stand down (R)"))
		{
			DraftSelected(false);
		}
		ImGui::TextUnformatted("Right click: move, or attack an enemy");
	}
	else if (!View.downed && ImGui::Button("Draft (R)"))
	{
		DraftSelected(true);
	}
}

// ---- The overlay -------------------------------------------------------------------------------

void UOrdersSubsystem::EnsureOverlayActor()
{
	if (OverlayActor)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	OverlayActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(OverlayActor, TEXT("OrdersRoot"));
	OverlayActor->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	OverlayActor->SetActorLabel(TEXT("Orders"));
#endif
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, OrdSolidMaterialPath);
	if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, OrdGhostMaterialPath))
	{
		const auto Make = [this, Parent](const FLinearColor& Color) -> UMaterialInstanceDynamic*
		{
			UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Parent, this);
			Material->SetVectorParameterValue(TEXT("Color"), Color);
			Material->SetScalarParameterValue(TEXT("Opacity"), Color.A);
			return Material;
		};
		ZoneMaterial = Make(FLinearColor(OrdZoneColor.R, OrdZoneColor.G, OrdZoneColor.B, 0.30f));
		GhostGoodMaterial = Make(FLinearColor(0.15f, 0.85f, 0.25f, 0.42f));
		GhostBadMaterial = Make(FLinearColor(0.95f, 0.18f, 0.15f, 0.42f));
	}
	else
	{
		UE_LOG(LogOrders, Warning, TEXT("%s not found: stockpile cells are drawn as outlines and no building ghost is shown. Run the BuildingMaterials commandlet (assets/manifest.json)."), OrdGhostMaterialPath);
	}
}

UStaticMesh* UOrdersSubsystem::GetMarkMesh(int32 Kind, int32 TopCm)
{
	const int32 Key = Kind * OrdKeyScale + TopCm / 50;
	if (const TObjectPtr<UStaticMesh>* Found = MarkMeshes.Find(Key))
	{
		return Found->Get();
	}
	TArray<VisualMesh::FPart> Parts;
	const auto Box = [&Parts](const FVector3f& Size, const FVector3f& At, const FLinearColor& Color)
	{
		VisualMesh::FPart Part;
		Part.Shape = VisualMesh::EShape::Box;
		Part.Size = Size;
		Part.At = At;
		Part.Color = Color;
		Parts.Add(Part);
	};
	const auto Ring = [&Box](const FLinearColor& Color)
	{
		Box(FVector3f(100.0f, 12.0f, 5.0f), FVector3f(0.0f, 44.0f, 0.0f), Color);
		Box(FVector3f(100.0f, 12.0f, 5.0f), FVector3f(0.0f, -44.0f, 0.0f), Color);
		Box(FVector3f(12.0f, 76.0f, 5.0f), FVector3f(44.0f, 0.0f, 0.0f), Color);
		Box(FVector3f(12.0f, 76.0f, 5.0f), FVector3f(-44.0f, 0.0f, 0.0f), Color);
	};
	switch (Kind)
	{
		case OrdMine:
			Ring(OrdMineColor);
			Box(FVector3f(14.0f, 14.0f, static_cast<float>(TopCm)), FVector3f::ZeroVector, OrdMineColor);
			break;
		case OrdCut:
		case OrdHarvest:
		{
			// A ring on the ground, a thin post and a plate above the plant, so it shows under a tree's crown from above.
			const FLinearColor& Color = Kind == OrdCut ? OrdCutColor : OrdHarvestColor;
			Ring(Color);
			Box(FVector3f(8.0f, 8.0f, static_cast<float>(TopCm)), FVector3f::ZeroVector, Color);
			Box(FVector3f(44.0f, 44.0f, 10.0f), FVector3f(0.0f, 0.0f, static_cast<float>(TopCm)), Color);
			break;
		}
		case OrdDeconstruct:
			Ring(OrdDeconstructColor);
			Box(FVector3f(70.0f, 70.0f, 8.0f), FVector3f::ZeroVector, OrdDeconstructColor);
			break;
		case OrdZoneTile:
			Box(FVector3f(96.0f, 96.0f, 3.0f), FVector3f::ZeroVector, OrdZoneColor);
			break;
		default:
			Ring(OrdZoneColor);
			break;
	}
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, nullptr, false);
	if (Mesh)
	{
		KeepMeshes.Add(Mesh);
		MarkMeshes.Add(Key, Mesh);
	}
	return Mesh;
}

UInstancedStaticMeshComponent* UOrdersSubsystem::GetOverlayComponent(int32 Key, UStaticMesh* Mesh, UMaterialInterface* Material)
{
	if (const TObjectPtr<UInstancedStaticMeshComponent>* Found = OverlayComponents.Find(Key))
	{
		return Found->Get();
	}
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(OverlayActor);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bAffectDistanceFieldLighting = false;
	if (Material)
	{
		Component->SetMaterial(0, Material);
	}
	Component->SetupAttachment(OverlayActor->GetRootComponent());
	Component->RegisterComponent();
	OverlayComponents.Add(Key, Component);
	return Component;
}

void UOrdersSubsystem::RebuildMarks(const USimSubsystem& Bridge)
{
	const colony::Sim& Sim = *Bridge.GetSim();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	const UBuildingSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingSubsystem>();
	TMap<int32, TArray<FTransform>> Batches;
	TMap<int32, UMaterialInterface*> Materials;
	Stats.MineMarks = Stats.CutMarks = Stats.HarvestMarks = Stats.DeconstructMarks = Stats.ZoneCells = 0;

	// How high a plant stands, from its def's stem and crown, cm.
	TMap<uint32, int32> PlantTop;
	const auto TopOfPlant = [&](colony::CellPos Cell) -> int32
	{
		const colony::DefId Def = Sim.plants().defAt(Cell);
		if (!Def.valid())
		{
			return 100;
		}
		if (const int32* Known = PlantTop.Find(Def.value))
		{
			return *Known;
		}
		float Top = 60.0f;
		VisualMesh::FPart Part;
		for (const char* Path : {"visual.stem", "visual.crown"})
		{
			if (VisualMesh::ReadPlantPart(Sim.defs().get(Def), Path, Part))
			{
				Top = FMath::Max(Top, Part.At.Z + Part.Size.Z);
			}
		}
		const int32 Quantised = FMath::Clamp(FMath::CeilToInt32((Top + 40.0f) / 50.0f) * 50, 100, 800);
		PlantTop.Add(Def.value, Quantised);
		return Quantised;
	};
	const auto Place = [&](int32 Kind, int32 TopCm, int32 X, int32 Y, double LiftCm, UMaterialInterface* Material)
	{
		const int32 Key = Kind * OrdKeyScale + TopCm / 50;
		const FVector Centre = Bridge.SimToWorld(static_cast<float>(X) + 0.5f, static_cast<float>(Y) + 0.5f, 0);
		Batches.FindOrAdd(Key).Add(FTransform(FRotator::ZeroRotator, FVector(Centre.X, Centre.Y, OrdCellTopCm(Bridge, X, Y) + LiftCm)));
		Materials.Add(Key, Material);
	};

	for (const colony::DesignationView& Mark : Snapshot.designations)
	{
		switch (Mark.kind)
		{
			case colony::DesignationKind::Mine:
				Place(OrdMine, OrdPostMineCm, Mark.cell.x, Mark.cell.y, 3.0, nullptr);
				++Stats.MineMarks;
				break;
			case colony::DesignationKind::CutPlant:
				Place(OrdCut, TopOfPlant(Mark.cell), Mark.cell.x, Mark.cell.y, 3.0, nullptr);
				++Stats.CutMarks;
				break;
			case colony::DesignationKind::HarvestPlant:
				Place(OrdHarvest, TopOfPlant(Mark.cell), Mark.cell.x, Mark.cell.y, 3.0, nullptr);
				++Stats.HarvestMarks;
				break;
			default:
				break;
		}
	}
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && View.markedForDeconstruct)
		{
			// On top of the building, where it can be seen.
			const float Height = Buildings ? Buildings->GetDrawnHeightCm(View.def) : 0.0f;
			Place(OrdDeconstruct, 0, FMath::FloorToInt32(View.x), FMath::FloorToInt32(View.y), static_cast<double>(Height > 0.0f ? Height : 250.0f) + 6.0, nullptr);
			++Stats.DeconstructMarks;
		}
	}
	UMaterialInterface* ZoneDraw = ZoneMaterial.Get();
	for (const colony::CellPos& Cell : Snapshot.zoneCells)
	{
		Place(ZoneDraw ? OrdZoneTile : OrdZoneRing, 0, Cell.x, Cell.y, 2.0, ZoneDraw);
		++Stats.ZoneCells;
	}

	for (TPair<int32, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : OverlayComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->ClearInstances();
		}
	}
	for (TPair<int32, TArray<FTransform>>& Pair : Batches)
	{
		const int32 Kind = Pair.Key / OrdKeyScale;
		const int32 TopCm = (Pair.Key % OrdKeyScale) * 50;
		UStaticMesh* Mesh = GetMarkMesh(Kind, TopCm);
		if (!Mesh)
		{
			continue;
		}
		UMaterialInterface* Material = Materials.FindRef(Pair.Key);
		if (UInstancedStaticMeshComponent* Component = GetOverlayComponent(Pair.Key, Mesh, Material))
		{
			Component->AddInstances(Pair.Value, false, true);
		}
	}
	++Stats.OverlayRebuilds;
}

void UOrdersSubsystem::RefreshOverlay()
{
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	EnsureOverlayActor();
	if (!OverlayActor)
	{
		return;
	}
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const bool bShow = GOrdersMarks.GetValueOnGameThread() != 0;
	for (TPair<int32, TObjectPtr<UInstancedStaticMeshComponent>>& Pair : OverlayComponents)
	{
		if (Pair.Value)
		{
			Pair.Value->SetVisibility(bShow);
		}
	}
	if (OverlaySeconds != Snapshot.gameSeconds)
	{
		OverlaySeconds = Snapshot.gameSeconds;
		// A signature of everything the marks show, so they are rebuilt only when it changes.
		uint64 Signature = OrdMix(0, static_cast<uint64>(Bridge->GetSimGeneration()));
		const FVector Origin = Bridge->GetWorldOrigin();
		Signature = OrdMix(Signature, static_cast<uint64>(static_cast<int64>(Origin.X)) ^ (static_cast<uint64>(static_cast<int64>(Origin.Y)) << 20) ^ (static_cast<uint64>(static_cast<int64>(Origin.Z)) << 40));
		for (const colony::DesignationView& Mark : Snapshot.designations)
		{
			Signature = OrdMix(Signature, OrdCellKey(Mark.cell.x, Mark.cell.y) + static_cast<uint64>(Mark.kind));
		}
		for (const colony::CellPos& Cell : Snapshot.zoneCells)
		{
			Signature = OrdMix(Signature, OrdCellKey(Cell.x, Cell.y) ^ 0x5A5A5A5AULL);
		}
		for (const colony::ThingView& View : Snapshot.things)
		{
			if (View.category == colony::ThingCategory::Building && View.stage == colony::ConstructionStage::Built && View.markedForDeconstruct)
			{
				Signature = OrdMix(Signature, static_cast<uint64>(static_cast<uint32>(View.id)) ^ 0xDEC0ULL);
			}
		}
		if (Signature != MarksSignature)
		{
			MarksSignature = Signature;
			RebuildMarks(*Bridge);
		}
	}
	UpdatePreview(*Bridge);
}

void UOrdersSubsystem::UpdatePreview(USimSubsystem& Bridge)
{
	UBuildingSubsystem* Buildings = GetWorld()->GetSubsystem<UBuildingSubsystem>();
	TArray<colony::CellPos> Origins;
	GetBuildOrigins(Origins);
	UStaticMesh* Mesh = Buildings && !Origins.IsEmpty() ? Buildings->GetWholeMesh(BuildDef, BuildStuff) : nullptr;
	if (!GhostGoodMaterial || !GhostBadMaterial)
	{
		Mesh = nullptr;
	}

	TArray<FTransform> Good;
	TArray<FTransform> Bad;
	uint64 Signature = 0;
	if (Mesh)
	{
		Signature = OrdMix(OrdMix(OrdMix(1, BuildDef.value), BuildStuff.value), static_cast<uint64>(BuildRot));
		for (const colony::CellPos& Origin : Origins)
		{
			FTransform Placed;
			if (!Buildings->GetPlacedTransform(BuildDef, Origin, BuildRot, Placed))
			{
				continue;
			}
			const bool bValid = CanPlace(BuildDef, Origin, BuildRot);
			(bValid ? Good : Bad).Add(Placed);
			Signature = OrdMix(Signature, OrdCellKey(Origin.x, Origin.y) + (bValid ? 1 : 0));
		}
	}
	if (Signature == PreviewSignature)
	{
		return;
	}
	PreviewSignature = Signature;

	const auto Fill = [this, Mesh](TObjectPtr<UInstancedStaticMeshComponent>& Component, UMaterialInterface* Material, const TArray<FTransform>& Transforms, int32& OutCount)
	{
		if (!Component && Mesh && Material)
		{
			Component = NewObject<UInstancedStaticMeshComponent>(OverlayActor);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			Component->bAffectDistanceFieldLighting = false;
			Component->SetupAttachment(OverlayActor->GetRootComponent());
			Component->RegisterComponent();
		}
		OutCount = 0;
		if (!Component)
		{
			return;
		}
		Component->ClearInstances();
		if (Mesh && !Transforms.IsEmpty())
		{
			if (Component->GetStaticMesh() != Mesh)
			{
				Component->SetStaticMesh(Mesh);
			}
			Component->SetMaterial(0, Material);
			Component->AddInstances(Transforms, false, true);
			OutCount = Transforms.Num();
		}
	};
	Fill(GhostGood, GhostGoodMaterial, Good, Stats.GhostValid);
	Fill(GhostBad, GhostBadMaterial, Bad, Stats.GhostInvalid);
}

void UOrdersSubsystem::DrawDragOutline(const USimSubsystem& Bridge) const
{
	UWorld* World = GetWorld();
	if (Tool == EOrderTool::None || !bHasHover)
	{
		return;
	}
	if (Tool == EOrderTool::Build)
	{
		// The footprint of each planned building: green where it can stand, red where it can't.
		TArray<colony::CellPos> Origins;
		GetBuildOrigins(Origins);
		const colony::Def& Def = Bridge.GetSim()->defs().get(BuildDef);
		for (const colony::CellPos& Origin : Origins)
		{
			const colony::CellRect Rect = colony::footprintRect(Origin, FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Def.numberAt("size", 0, 1.0)))),
				FMath::Max(1, FMath::RoundToInt32(static_cast<float>(Def.numberAt("size", 1, 1.0)))), BuildRot);
			OrdDrawRect(World, Bridge, Rect, CanPlace(BuildDef, Origin, BuildRot) ? FColor(60, 230, 90) : FColor(240, 60, 50), 4.0f);
		}
		return;
	}
	OrdDrawRect(World, Bridge, DragRect(), OrdToolColor(Tool), bDragging ? 6.0f : 3.0f);
}

void UOrdersSubsystem::DrawDraftMarkers(const USimSubsystem& Bridge) const
{
	UWorld* World = GetWorld();
	const colony::Snapshot& Snapshot = Bridge.GetCurrentSnapshot();
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Pawn || !View.drafted || View.faction != 1 || View.dead)
		{
			continue;
		}
		FVector Feet;
		if (!Bridge.GetBlendedLocation(View.id, Feet))
		{
			continue;
		}
		DrawDebugCircle(World, Feet + FVector(0.0, 0.0, 12.0), 55.0f, 24, FColor(235, 50, 40), false, -1.0f, SDPG_Foreground, 4.0f, FVector(1.0, 0.0, 0.0), FVector(0.0, 1.0, 0.0), false);
		const FVector Spot = Bridge.SimToWorld(View.fightSpot.x + 0.5f, View.fightSpot.y + 0.5f, View.level);
		if (View.fighting && FVector::DistSquared2D(Spot, Feet) > 150.0 * 150.0)
		{
			DrawDebugLine(World, Feet + FVector(0.0, 0.0, 20.0), Spot + FVector(0.0, 0.0, 20.0), FColor(240, 220, 60), false, -1.0f, SDPG_Foreground, 2.0f);
		}
		FVector Target;
		if (View.fightTarget != colony::kNoEntity && Bridge.GetBlendedLocation(View.fightTarget, Target))
		{
			DrawDebugLine(World, Feet + FVector(0.0, 0.0, 120.0), Target + FVector(0.0, 0.0, 120.0), FColor(235, 50, 40), false, -1.0f, SDPG_Foreground, 2.0f);
		}
	}
}

// ---- The tick and the panels -------------------------------------------------------------------

void UOrdersSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (ContentGeneration != Bridge->GetSimGeneration())
	{
		ContentGeneration = Bridge->GetSimGeneration();
		ResetForNewSim();
	}
	RefreshOverlay();

	if (!IsOverseerActive())
	{
		// Walking the colony (U10): the Build tool stays, aimed by the crosshair (UHandsOnSubsystem), and draws its outline.
		if (IsDirectBuilding())
		{
			if (Bridge->GetCurrentSnapshot().gameSeconds != StuffOptionsSeconds)
			{
				RefreshStuffOptions();
			}
			DrawDragOutline(*Bridge);
			return;
		}
		CancelTool();
		return;
	}
	if (bMouseDriven && Tool != EOrderTool::None && !bMouseOverUi)
	{
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		const APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		if (PC && PC->GetMousePosition(MouseX, MouseY))
		{
			const FSimPick Pick = PickGroundAt(FVector2D(MouseX, MouseY));
			if (Pick.Kind == ESimPickKind::Cell)
			{
				SetHoverCell(Pick.Cell);
			}
		}
	}
	if (Tool == EOrderTool::Build && Bridge->GetCurrentSnapshot().gameSeconds != StuffOptionsSeconds)
	{
		RefreshStuffOptions();
	}
	DrawDragOutline(*Bridge);
	DrawDraftMarkers(*Bridge);
	IConsoleVariable* ShowUi = IConsoleManager::Get().FindConsoleVariable(TEXT("Overseer.UI"));
	if (!ShowUi || ShowUi->GetInt() != 0)
	{
		DrawPanels(*Bridge);
	}
}

void UOrdersSubsystem::DrawPanels(USimSubsystem& Bridge)
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
	const colony::Sim& Sim = *Bridge.GetSim();
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	// ImGui knows from the last frame whether the cursor is over one of its windows.
	bMouseOverUi = ImGui::GetIO().WantCaptureMouse;

	// The alarm, top right.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x - Margin, Viewport->Pos.y + Margin), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	if (ImGui::Begin("##OrdersAlarm", nullptr, PanelFlags))
	{
		bool bAnyColonist = false;
		bool bAnyUndrafted = false;
		for (const colony::ThingView& View : Bridge.GetCurrentSnapshot().things)
		{
			if (View.category == colony::ThingCategory::Pawn && View.faction == 1 && !View.dead && !View.downed && Sim.colonistOf(View.id))
			{
				bAnyColonist = true;
				bAnyUndrafted = bAnyUndrafted || !View.drafted;
			}
		}
		ImGui::PushStyleColor(ImGuiCol_Button, bAnyUndrafted ? ImVec4(0.72f, 0.14f, 0.12f, 1.0f) : ImVec4(0.22f, 0.45f, 0.30f, 1.0f));
		if (ImGui::Button(bAnyUndrafted ? "ALARM" : "Stand down") && bAnyColonist)
		{
			Alarm();
		}
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("%s", bAnyUndrafted ? "Draft every colonist" : "Send every colonist back to work");
		}
	}
	ImGui::End();

	// The architect bar, bottom middle.
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	if (ImGui::Begin("##OrdersBar", nullptr, PanelFlags))
	{
		if (!Message.IsEmpty() && FPlatformTime::Seconds() < MessageUntil)
		{
			OrdText(Message);
		}
		// Categories.
		const auto Category = [this](const FString& Name)
		{
			if (OrdButton(Name, OpenCategory == Name))
			{
				OpenCategory = OpenCategory == Name ? FString() : Name;
			}
			ImGui::SameLine();
		};
		Category(TEXT("Orders"));
		Category(TEXT("Zones"));
		for (const FString& Name : BuildCategories)
		{
			Category(Name);
		}
		if (Tool != EOrderTool::None && OrdButton(TEXT("Stop (Esc)"), false))
		{
			CancelTool();
		}
		ImGui::NewLine();

		// What is in the open category.
		const auto ToolButton = [this](const TCHAR* Label, EOrderTool ForTool)
		{
			if (OrdButton(Label, Tool == ForTool))
			{
				SetTool(Tool == ForTool ? EOrderTool::None : ForTool);
			}
			ImGui::SameLine();
		};
		if (OpenCategory == TEXT("Orders"))
		{
			ToolButton(TEXT("Mine"), EOrderTool::Mine);
			ToolButton(TEXT("Chop trees"), EOrderTool::Chop);
			ToolButton(TEXT("Harvest"), EOrderTool::Harvest);
			ToolButton(TEXT("Deconstruct"), EOrderTool::Deconstruct);
			ToolButton(TEXT("Cancel"), EOrderTool::Cancel);
			ImGui::NewLine();
		}
		else if (OpenCategory == TEXT("Zones"))
		{
			ToolButton(TEXT("Stockpile"), EOrderTool::Stockpile);
			ToolButton(TEXT("Remove zone"), EOrderTool::RemoveZone);
			ImGui::NewLine();
		}
		else if (!OpenCategory.IsEmpty())
		{
			for (const FBuildOption& Option : BuildOptions)
			{
				if (Option.Category != OpenCategory)
				{
					continue;
				}
				if (OrdButton(Option.Label, Tool == EOrderTool::Build && BuildDef == Option.Def))
				{
					if (Tool == EOrderTool::Build && BuildDef == Option.Def)
					{
						CancelTool();
					}
					else
					{
						SelectBuilding(UTF8_TO_TCHAR(Sim.defs().get(Option.Def).defName().c_str()));
					}
				}
				ImGui::SameLine();
			}
			ImGui::NewLine();
		}

		// Details of the tool in hand.
		if (Tool == EOrderTool::Build && BuildDef.valid())
		{
			const colony::Construction& Construction = Sim.construction();
			if (!StuffOptions.IsEmpty())
			{
				FString Preview = TEXT("(none)");
				for (const FStuffOption& Option : StuffOptions)
				{
					if (Option.Def == BuildStuff)
					{
						Preview = FString::Printf(TEXT("%s (%d)"), *Option.Label, Option.InStock);
					}
				}
				OrdText(TEXT("Made of"));
				ImGui::SameLine();
				ImGui::SetNextItemWidth(200.0f * Scale);
				if (ImGui::BeginCombo("##OrdersStuff", TCHAR_TO_UTF8(*Preview)))
				{
					for (const FStuffOption& Option : StuffOptions)
					{
						const FString Label = FString::Printf(TEXT("%s (%d)"), *Option.Label, Option.InStock);
						if (ImGui::Selectable(TCHAR_TO_UTF8(*Label), Option.Def == BuildStuff))
						{
							BuildStuff = Option.Def;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
			}
			if (ImGui::Button("Rotate (R)"))
			{
				RotateBuilding();
			}
			FString Cost;
			for (const colony::Construction::Cost& Part : Construction.costOf(BuildDef, BuildStuff))
			{
				Cost += FString::Printf(TEXT("%s%d %s"), Cost.IsEmpty() ? TEXT("") : TEXT(", "), Part.count, UTF8_TO_TCHAR(Sim.defs().get(Part.item).label().c_str()));
			}
			ImGui::SameLine();
			OrdText(FString::Printf(TEXT("Costs %s.  Click to plan; drag a line for walls."), *Cost));
		}
		else if (Tool != EOrderTool::None)
		{
			OrdText(TEXT("Drag a rectangle on the ground.  Right click or Esc stops."));
		}
	}
	ImGui::End();
}

#include "OrdersSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "UnrealClient.h"
#include "Widgets/SViewport.h"
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

#include "ColonyHudStyle.h"
#include "HudIcons.h"

#include "BuildingSubsystem.h"
#include "LevelsSubsystem.h"
#include "OverseerPawn.h"
#include "OverseerSubsystem.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "VisualMesh.h"

#include "ColonySim/BuildPlanner.h"
#include "ColonySim/Construction.h"
#include "ColonySim/Designations.h"
#include "ColonySim/Farming.h"
#include "ColonySim/Floors.h"
#include "ColonySim/Map.h"
#include "ColonySim/Plants.h"
#include "ColonySim/Research.h"
#include "ColonySim/Sim.h"
#include "ColonySim/UpperFloors.h"

#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogOrders, Log, All);

static TAutoConsoleVariable<int32> CVarOrdersClickFirst(TEXT("Orders.ClickFirst"), 1,
	TEXT("1: a left press on the world with an orders tool in hand is taken ahead of the ImGui overlay whenever no ImGui window is under the cursor. ")
	TEXT("0: the old way, through ImGui's want-the-mouse flag and the overseer pawn's key binding (smoke test orders_world shows the difference)."));

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
	constexpr int32 OrdGrowTile = 7;
	constexpr int32 OrdGrowRing = 8;
	constexpr int32 OrdLandTile = 9;
	constexpr int32 OrdLandRing = 10;
	constexpr int32 OrdAreaRing = 11;
	constexpr int32 OrdAnimalRing = 12;
	constexpr int32 OrdKeyScale = 10000;

	constexpr int32 OrdMaxBuildOrigins = 600;
	// The tiles' hotkeys, in tile order: letters the overseer view uses for nothing else (not B build, I inventory,
	// O overseer, R turn, Y take over).
	const TCHAR* const OrdTileKeys = TEXT("ZXCVNMGHJKLUPT");
	constexpr int32 OrdPostMineCm = 150;
	constexpr double OrdMessageSeconds = 4.0;

	const FLinearColor OrdMineColor = FLinearColor::FromSRGBColor(FColor(255, 140, 30));
	const FLinearColor OrdCutColor = FLinearColor::FromSRGBColor(FColor(70, 210, 70));
	const FLinearColor OrdHarvestColor = FLinearColor::FromSRGBColor(FColor(245, 215, 50));
	const FLinearColor OrdDeconstructColor = FLinearColor::FromSRGBColor(FColor(225, 50, 50));
	const FLinearColor OrdZoneColor = FLinearColor::FromSRGBColor(FColor(60, 150, 230));
	const FLinearColor OrdGrowColor = FLinearColor::FromSRGBColor(FColor(120, 200, 60));
	const FLinearColor OrdLandColor = FLinearColor::FromSRGBColor(FColor(250, 185, 40));
	const FLinearColor OrdAreaColor = FLinearColor::FromSRGBColor(FColor(120, 150, 240));
	const FLinearColor OrdAnimalColor = FLinearColor::FromSRGBColor(FColor(205, 150, 90));

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
		return colony::CellRect{MinX, MinY, FMath::Abs(A.x - B.x) + 1, FMath::Abs(A.y - B.y) + 1, A.level};
	}

	/** The cells a building, plan or frame covers, from its snapshot entry. */
	colony::CellRect OrdThingRect(const colony::ThingView& View)
	{
		return colony::CellRect{FMath::RoundToInt32(View.x - static_cast<float>(View.sizeX) * 0.5f), FMath::RoundToInt32(View.y - static_cast<float>(View.sizeY) * 0.5f),
			FMath::Max(1, View.sizeX), FMath::Max(1, View.sizeY), View.level};
	}

	bool OrdOverlap(const colony::CellRect& A, const colony::CellRect& B)
	{
		return A.level == B.level && A.minX < B.minX + B.sizeX && B.minX < A.minX + A.sizeX && A.minY < B.minY + B.sizeY && B.minY < A.minY + A.sizeY;
	}

	/** A corner of a cell on the surface that is drawn (the rock block included; an upper level's floor height), lifted a little so lines don't sink into the ground. */
	FVector OrdCorner(const USimSubsystem& Bridge, int32 X, int32 Y, int32 Level)
	{
		const colony::Map& Map = Bridge.GetSim()->map();
		FVector Point = Bridge.SimToWorld(FMath::Clamp(static_cast<float>(X), 0.0f, static_cast<float>(Map.width()) - 0.01f),
			FMath::Clamp(static_cast<float>(Y), 0.0f, static_cast<float>(Map.height()) - 0.01f), Level);
		double Bonus = 0.0;
		for (int32 DY = -1; DY <= 0 && Level == 0; ++DY)
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
			DrawDebugLine(World, OrdCorner(Bridge, X0, Y0, Rect.level), OrdCorner(Bridge, X1, Y1, Rect.level), Color, false, -1.0f, SDPG_Foreground, Thickness);
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
			case EOrderTool::Grow: return FColor(120, 200, 60);
			case EOrderTool::LandingZone: return FColor(250, 185, 40);
			case EOrderTool::RemoveLandingZone: return FColor(160, 160, 170);
			case EOrderTool::Cancel: return FColor(255, 255, 255);
			case EOrderTool::Deconstruct: return FColor(225, 50, 50);
			case EOrderTool::RemoveFloor: return FColor(200, 120, 60);
			case EOrderTool::LevelEarth: return FColor(170, 120, 70);
			case EOrderTool::CancelEarth: return FColor(160, 160, 170);
			case EOrderTool::PaintArea: return FColor(120, 150, 240);
			case EOrderTool::EraseArea: return FColor(160, 160, 170);
			case EOrderTool::AnimalZone: return FColor(205, 150, 90);
			case EOrderTool::Tame: return FColor(120, 200, 230);
			case EOrderTool::Hunt: return FColor(230, 90, 70);
			case EOrderTool::Slaughter: return FColor(190, 60, 60);
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

	/** What the strip above the tiles calls a tool. */
	const TCHAR* OrdToolLabel(EOrderTool Tool)
	{
		switch (Tool)
		{
			case EOrderTool::Stockpile: return TEXT("Stockpile zone");
			case EOrderTool::RemoveZone: return TEXT("Remove zone");
			case EOrderTool::Mine: return TEXT("Mine");
			case EOrderTool::Chop: return TEXT("Chop trees");
			case EOrderTool::Harvest: return TEXT("Harvest");
			case EOrderTool::Cancel: return TEXT("Cancel");
			case EOrderTool::Deconstruct: return TEXT("Deconstruct");
			case EOrderTool::Grow: return TEXT("Growing zone");
			case EOrderTool::RemoveFloor: return TEXT("Remove floor");
			case EOrderTool::AnimalZone: return TEXT("Animal zone");
			case EOrderTool::Tame: return TEXT("Tame");
			case EOrderTool::Hunt: return TEXT("Hunt");
			case EOrderTool::Slaughter: return TEXT("Slaughter");
			default: return TEXT("Build");
		}
	}

	const TCHAR* OrdToolName(EOrderTool Tool)
	{
		switch (Tool)
		{
			case EOrderTool::None: return TEXT("None");
			case EOrderTool::Build: return TEXT("Build");
			case EOrderTool::Stockpile: return TEXT("Stockpile");
			case EOrderTool::RemoveZone: return TEXT("RemoveZone");
			case EOrderTool::Mine: return TEXT("Mine");
			case EOrderTool::Chop: return TEXT("Chop");
			case EOrderTool::Harvest: return TEXT("Harvest");
			case EOrderTool::Cancel: return TEXT("Cancel");
			case EOrderTool::Deconstruct: return TEXT("Deconstruct");
			case EOrderTool::Grow: return TEXT("Grow");
			case EOrderTool::RemoveFloor: return TEXT("RemoveFloor");
			case EOrderTool::LandingZone: return TEXT("LandingZone");
			case EOrderTool::RemoveLandingZone: return TEXT("RemoveLandingZone");
			case EOrderTool::LevelEarth: return TEXT("LevelEarth");
			case EOrderTool::CancelEarth: return TEXT("CancelEarth");
			case EOrderTool::PaintArea: return TEXT("PaintArea");
			case EOrderTool::EraseArea: return TEXT("EraseArea");
			case EOrderTool::AnimalZone: return TEXT("AnimalZone");
			case EOrderTool::Tame: return TEXT("Tame");
			case EOrderTool::Hunt: return TEXT("Hunt");
			case EOrderTool::Slaughter: return TEXT("Slaughter");
		}
		return TEXT("?");
	}

	/** The painted icon of a tool (assets/ui/icons), empty for one that has none. */
	const TCHAR* OrdToolIcon(EOrderTool Tool)
	{
		switch (Tool)
		{
			case EOrderTool::Stockpile: return TEXT("zone_stockpile");
			case EOrderTool::RemoveZone: return TEXT("zone_remove");
			case EOrderTool::Mine: return TEXT("order_mine");
			case EOrderTool::Chop: return TEXT("order_chop");
			case EOrderTool::Harvest: return TEXT("order_harvest");
			case EOrderTool::Cancel: return TEXT("order_cancel");
			case EOrderTool::Deconstruct: return TEXT("order_deconstruct");
			case EOrderTool::Grow: return TEXT("zone_grow");
			case EOrderTool::RemoveFloor: return TEXT("order_remove_floor");
			case EOrderTool::LandingZone: return TEXT("zone_landing");
			case EOrderTool::RemoveLandingZone: return TEXT("zone_remove_landing");
			case EOrderTool::LevelEarth: return TEXT("earth_level");
			case EOrderTool::CancelEarth: return TEXT("earth_cancel");
			case EOrderTool::PaintArea: return TEXT("area_paint");
			case EOrderTool::EraseArea: return TEXT("area_erase");
			case EOrderTool::AnimalZone: return TEXT("zone_animal");
			case EOrderTool::Tame: return TEXT("order_tame");
			case EOrderTool::Hunt: return TEXT("order_hunt");
			case EOrderTool::Slaughter: return TEXT("order_slaughter");
			default: return TEXT("");
		}
	}

	/** The painted icon of a Build menu category: "cat_" and its name in lower case ("cat_structure", "cat_recreation"). */
	FString OrdCategoryIcon(const FString& Category)
	{
		return TEXT("cat_") + Category.ToLower().Replace(TEXT(" "), TEXT("_"));
	}

	void OrdText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

namespace
{
	/**
	 * Left button events for the orders tools, before the ImGui overlay's own input processor sees them. That processor swallows
	 * every mouse press while ImGui's io.WantCaptureMouse is set, and that flag also stays set while ImGui believes some mouse button
	 * is still held (a release it never got), so a press on the ground with a tool in hand could die there without a word, while
	 * the bar's own buttons kept working. Here a world press needs only that no ImGui window or popup is under the cursor.
	 */
	class FOrdersClickProcessor : public IInputProcessor
	{
	public:
		explicit FOrdersClickProcessor(UOrdersSubsystem* InOwner) : Owner(InOwner) {}

		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual bool HandleMouseButtonDownEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
		{
			UOrdersSubsystem* Orders = Owner.Get();
			return Orders && Event.GetEffectingButton() == EKeys::LeftMouseButton && Orders->HandleSlateLeftButton(true, FVector2D(Event.GetScreenSpacePosition()));
		}

		virtual bool HandleMouseButtonDoubleClickEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
		{
			// A quick second click arrives as a double click; for a tool it is just another press.
			return HandleMouseButtonDownEvent(SlateApp, Event);
		}

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& Event) override
		{
			UOrdersSubsystem* Orders = Owner.Get();
			if (Orders && Event.GetEffectingButton() == EKeys::LeftMouseButton)
			{
				Orders->HandleSlateLeftButton(false, FVector2D(Event.GetScreenSpacePosition()));
			}
			// Everyone else (ImGui, the viewport) still gets the release.
			return false;
		}

		virtual const TCHAR* GetDebugName() const override { return TEXT("OrdersClickProcessor"); }

	private:
		TWeakObjectPtr<UOrdersSubsystem> Owner;
	};
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
	if (ClickProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(ClickProcessor);
	}
	ClickProcessor.Reset();
	OverlayActor = nullptr;
	OverlayComponents.Reset();
	GhostGood = nullptr;
	GhostBad = nullptr;
	IconStage = nullptr;
	IconCapture = nullptr;
	IconMesh = nullptr;
	TileIcons.Reset();
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
	// New defs and new building meshes: the tile pictures are made again (the render targets are kept and drawn over).
	WantedTileIcons.Reset();
	NoTileIcon.Reset();
	TileIconsReady.Reset();
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
	// The sim's own menu (docs/design/build-menu.md): categories in RimWorld's order, floors on Floors, locked entries kept.
	const colony::BuildPlanner Planner(Sim);
	for (const colony::BuildMenuCategory& Category : Planner.menu())
	{
		const FString Label = OrdCapital(Category.label);
		BuildCategories.AddUnique(Label);
		for (const colony::BuildMenuEntry& Entry : Category.entries)
		{
			FBuildOption Option;
			Option.Def = Sim.defs().idOf(Entry.floor ? "Terrain" : "Things", Entry.defName);
			if (!Option.Def.valid())
			{
				continue;
			}
			Option.DefName = UTF8_TO_TCHAR(Entry.defName.c_str());
			Option.Label = OrdCapital(Entry.label);
			Option.Category = Label;
			Option.bFloor = Entry.floor;
			Option.Locked = UTF8_TO_TCHAR(Entry.locked.c_str());
			Option.Drag = Entry.drag;
			Option.SizeX = FMath::Max(1, Entry.sizeX);
			Option.SizeY = FMath::Max(1, Entry.sizeY);
			BuildOptions.Add(Option);
		}
	}
	int32 Done = 0;
	for (const colony::ResearchView& Project : Bridge->GetCurrentSnapshot().research)
	{
		Done += Project.done ? 1 : 0;
	}
	ResearchDoneSeen = Done;
}

const UOrdersSubsystem::FBuildOption* UOrdersSubsystem::FindOption(colony::DefId Def) const
{
	return BuildOptions.FindByPredicate([Def](const FBuildOption& Option) { return Option.Def == Def; });
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
	StuffOptionsSeconds = Bridge->GetCurrentSnapshot().gameSeconds;
	// The stuffs that fit, the most on the map first (BuildPlanner::stuffChoices), for buildings and floors alike.
	for (const colony::StuffChoice& Choice : colony::BuildPlanner(Sim).stuffChoices(Sim.defs().get(BuildDef).defName()))
	{
		FStuffOption Option;
		Option.Def = Sim.defs().idOf("Things", Choice.defName);
		Option.Label = OrdCapital(Choice.label);
		Option.InStock = Choice.onMap;
		if (Option.Def.valid())
		{
			StuffOptions.Add(Option);
		}
	}
}

colony::DefId UOrdersSubsystem::BestStuffFor(colony::DefId Def) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Def.valid())
	{
		return colony::DefId();
	}
	const colony::Sim& Sim = *Bridge->GetSim();
	const std::string Best = colony::BuildPlanner(Sim).bestStuff(Sim.defs().get(Def).defName());
	return Best.empty() ? colony::DefId() : Sim.defs().idOf("Things", Best);
}

// ---- Tools -------------------------------------------------------------------------------------

bool UOrdersSubsystem::SetGrowCrop(const FString& DefName)
{
	const USimSubsystem* Bridge = GetBridge();
	const colony::Sim* Sim = Bridge ? Bridge->GetSim() : nullptr;
	if (!Sim)
	{
		return false;
	}
	const colony::DefId Crop = Sim->defs().idOf("Things", TCHAR_TO_UTF8(*DefName));
	if (!Sim->farming().isCrop(Crop) || !Sim->research().unlocked(Sim->defs().get(Crop)))
	{
		return false;
	}
	GrowCrop = Crop;
	return true;
}

void UOrdersSubsystem::SetTool(EOrderTool NewTool)
{
	bDragging = false;
	Tool = NewTool;
	if (Tool == EOrderTool::Build && !BuildDef.valid())
	{
		for (const FBuildOption& Option : BuildOptions)
		{
			if (Option.Locked.IsEmpty())
			{
				BuildDef = Option.Def;
				BuildShape = Option.Drag;
				BuildStuff = BestStuffFor(BuildDef);
				RefreshStuffOptions();
				break;
			}
		}
	}
}

void UOrdersSubsystem::SetAreaTool(uint32 Area, bool bAdd)
{
	AreaToolArea = Area;
	SetTool(Area == 0 ? EOrderTool::None : (bAdd ? EOrderTool::PaintArea : EOrderTool::EraseArea));
	// The overlay shows the area's cells now, so it is worked out again at once.
	OverlaySeconds = -1;
}

bool UOrdersSubsystem::SelectBuilding(const FString& DefName, const FString& Stuff)
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return false;
	}
	const FBuildOption* Option = BuildOptions.FindByPredicate([&DefName](const FBuildOption& Entry) { return Entry.DefName == DefName; });
	if (!Option)
	{
		return false;
	}
	if (!Option->Locked.IsEmpty())
	{
		Say(FString::Printf(TEXT("%s needs research first: %s"), *Option->Label, *Option->Locked));
		return false;
	}
	const colony::DefDatabase& Defs = Bridge->GetSim()->defs();
	const colony::DefId Id = Option->Def;
	const colony::DragShape Shape = Option->Drag;
	bDragging = false;
	Tool = EOrderTool::Build;
	BuildDef = Id;
	BuildShape = Shape;
	BuildRot = colony::Rot4::North;
	RefreshStuffOptions();
	BuildStuff = BestStuffFor(Id);
	if (!Stuff.IsEmpty())
	{
		const colony::DefId Wanted = Defs.idOf("Things", TCHAR_TO_UTF8(*Stuff));
		if (StuffOptions.ContainsByPredicate([Wanted](const FStuffOption& Choice) { return Choice.Def == Wanted; }))
		{
			BuildStuff = Wanted;
		}
	}
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
	const int32 Count = BuildOptions.Num();
	int32 Index = BuildOptions.IndexOfByPredicate([this](const FBuildOption& Option) { return Option.Def == BuildDef; });
	for (int32 Tries = 0; Tries < Count; ++Tries)
	{
		Index = Index < 0 ? (Direction >= 0 ? 0 : Count - 1) : (Index + (Direction >= 0 ? 1 : -1) + Count) % Count;
		if (BuildOptions[Index].Locked.IsEmpty())
		{
			return SelectBuilding(BuildOptions[Index].DefName);
		}
	}
	return false;
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

bool UOrdersSubsystem::ScreenToViewport(const FVector2D& ScreenSpace, FVector2D& OutPixel) const
{
	const UWorld* World = GetWorld();
	UGameViewportClient* Client = World ? World->GetGameViewport() : nullptr;
	const TSharedPtr<SViewport> Widget = Client ? Client->GetGameViewportWidget() : nullptr;
	if (!Widget || !Client->Viewport)
	{
		return false;
	}
	const FGeometry& Geometry = Widget->GetTickSpaceGeometry();
	const FVector2D LocalSize = FVector2D(Geometry.GetLocalSize());
	const FIntPoint Size = Client->Viewport->GetSizeXY();
	if (LocalSize.X <= 0.0 || LocalSize.Y <= 0.0 || Size.X <= 0 || Size.Y <= 0)
	{
		return false;
	}
	OutPixel = FVector2D(Geometry.AbsoluteToLocal(ScreenSpace)) * FVector2D(Size) / LocalSize;
	return OutPixel.X >= 0.0 && OutPixel.Y >= 0.0 && OutPixel.X < Size.X && OutPixel.Y < Size.Y;
}

bool UOrdersSubsystem::CursorViewportPosition(FVector2D& OutPixel) const
{
	return FSlateApplication::IsInitialized() && ScreenToViewport(FVector2D(FSlateApplication::Get().GetCursorPos()), OutPixel);
}

bool UOrdersSubsystem::HandleSlateLeftButton(bool bPressed, const FVector2D& ScreenSpace)
{
	if (bPressed)
	{
		FVector2D Pixel;
		if (CVarOrdersClickFirst.GetValueOnGameThread() == 0 || Tool == EOrderTool::None || !IsOverseerActive() || bMouseOverUi || !ScreenToViewport(ScreenSpace, Pixel))
		{
			return false;
		}
		return HandleLeftPressed(Pixel);
	}
	if (!bDragging)
	{
		return false;
	}
	FVector2D Pixel;
	if (!ScreenToViewport(ScreenSpace, Pixel))
	{
		// Let go outside the game's view: the drag ends at the last cell it was over.
		UE_LOG(LogOrders, Log, TEXT("Left release outside the view: tool %s, ends at the last hovered cell"), OrdToolName(Tool));
		EndDrag(Hover);
		return true;
	}
	return HandleLeftReleased(Pixel);
}

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
	// On the level the overseer looks at (docs/design/levels-draw.md).
	return SimPicking::PickGroundFromRay(*Bridge, Origin, Direction, ULevelsSubsystem::TopShownLevel(World));
}

bool UOrdersSubsystem::HandleLeftPressed(const FVector2D& Screen)
{
	if (Tool == EOrderTool::None || !IsOverseerActive())
	{
		UE_LOG(LogOrders, Verbose, TEXT("Left press at (%.0f, %.0f): no tool in hand (tool %s, overseer %s)"), Screen.X, Screen.Y,
			OrdToolName(Tool), IsOverseerActive() ? TEXT("on") : TEXT("off"));
		return false;
	}
	const FSimPick Pick = PickGroundAt(Screen);
	UE_LOG(LogOrders, Log, TEXT("Left press at (%.0f, %.0f): tool %s, ground %s"), Screen.X, Screen.Y, OrdToolName(Tool),
		Pick.Kind == ESimPickKind::Cell ? *FString::Printf(TEXT("cell %d, %d, %d"), Pick.Cell.x, Pick.Cell.y, Pick.Cell.level) : TEXT("none"));
	if (Pick.Kind == ESimPickKind::Cell)
	{
		BeginDrag(Pick.Cell);
	}
	else
	{
		// Say why nothing happens instead of failing silently (off the map, the sky, or the camera has no view).
		Say(TEXT("Click on the ground"));
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
	UE_LOG(LogOrders, Log, TEXT("Left release at (%.0f, %.0f): tool %s, ground %s"), Screen.X, Screen.Y, OrdToolName(Tool),
		Pick.Kind == ESimPickKind::Cell ? *FString::Printf(TEXT("cell %d, %d, %d"), Pick.Cell.x, Pick.Cell.y, Pick.Cell.level) : TEXT("none (ends at the last hovered cell)"));
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
	const int32 Level = Pick.Kind == ESimPickKind::Cell ? Pick.Cell.level : 0;
	const colony::CellPos Cell{FMath::FloorToInt32(Local.X), FMath::FloorToInt32(Local.Y), Level};
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
	if (Tool == EOrderTool::LevelEarth)
	{
		LevelStartCm = LevelHeightAtCm(Cell);
	}
}

int32 UOrdersSubsystem::LevelHeightAtCm(colony::CellPos Cell) const
{
	// The ground in the middle of the cell, on the 25 cm steps every base keeps (docs/design/earthworks.md).
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim())
	{
		return 0;
	}
	const colony::Map& Map = Bridge->GetSim()->map();
	const float Metres = Map.heightAt(static_cast<float>(Cell.x) + 0.5f, static_cast<float>(Cell.y) + 0.5f);
	return FMath::RoundToInt32(Metres / colony::Map::kLevelStepM) * FMath::RoundToInt32(colony::Map::kLevelStepM * 100.0f);
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
	const FBuildOption* Option = FindOption(BuildDef);
	const int32 SizeX = Option ? Option->SizeX : 1;
	const int32 SizeY = Option ? Option->SizeY : 1;
	const colony::CellRect Foot = colony::footprintRect(colony::CellPos{0, 0, 0}, SizeX, SizeY, BuildRot);
	if (BuildShape == colony::DragShape::Single)
	{
		// One, with its footprint centred on the cursor's cell.
		Out.Add(colony::CellPos{Hover.x - (Foot.sizeX - 1) / 2, Hover.y - (Foot.sizeY - 1) / 2, Hover.level});
		return;
	}
	// A line, a room's outline or a filled rectangle from where the drag started (BuildPlanner::dragCells, the sim's own shapes).
	const colony::CellPos From = bDragging ? DragStart : Hover;
	for (colony::CellPos Cell : colony::BuildPlanner::dragCells(From, Hover, BuildShape, Foot.sizeX, Foot.sizeY))
	{
		if (Out.Num() >= OrdMaxBuildOrigins)
		{
			break;
		}
		Cell.level = Hover.level;
		Out.Add(Cell);
	}
}

bool UOrdersSubsystem::CanPlace(colony::DefId Def, colony::CellPos Origin, colony::Rot4 Rot) const
{
	const USimSubsystem* Bridge = GetBridge();
	if (!Bridge || !Bridge->GetSim() || !Def.valid())
	{
		return false;
	}
	// The sim's own rule for the plan (docs/design/build-menu.md): map edge, water, rock, a building, a plan, a floor already laid,
	// levels and climbing.
	const colony::Construction& Construction = Bridge->GetSim()->construction();
	const colony::DefId Stuff = Def == BuildDef ? BuildStuff : colony::DefId();
	const FBuildOption* Option = FindOption(Def);
	return Option && Option->bFloor ? Construction.whyNotFloorPlan(Def, Stuff, Origin).empty() : Construction.whyNotBlueprint(Def, Stuff, Origin, Rot).empty();
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
			// Stockpile, growing and animal zones alike (each command fails quietly where it finds no zone of its kind).
			Submit(colony::RemoveZoneCellsCommand{Rect});
			Submit(colony::RemoveGrowingZoneCellsCommand{Rect});
			for (const colony::CellPos& Cell : Snapshot.animalZoneCells)
			{
				if (Rect.contains(Cell))
				{
					Submit(colony::RemoveAnimalZoneCellsCommand{Rect});
					break;
				}
			}
			Say(TEXT("Zone cells removed"));
			break;
		case EOrderTool::Grow:
		{
			if (!GrowCrop.valid())
			{
				SetGrowCrop(TEXT("Plant_Rice"));
			}
			if (!GrowCrop.valid())
			{
				Say(TEXT("No crop to sow"));
				break;
			}
			const colony::Def& Crop = Bridge->GetSim()->defs().get(GrowCrop);
			Submit(colony::CreateGrowingZoneCommand{Rect, Crop.defName()});
			Say(FString::Printf(TEXT("Growing zone %d x %d: %s"), Rect.sizeX, Rect.sizeY, *OrdCapital(Crop.label())));
			break;
		}
		case EOrderTool::LandingZone:
		case EOrderTool::RemoveLandingZone:
		{
			const bool bAdd = Tool == EOrderTool::LandingZone;
			Submit(colony::SetLandingZoneCommand{Rect, bAdd});
			Say(bAdd ? FString::Printf(TEXT("Landing zone %d x %d"), Rect.sizeX, Rect.sizeY) : FString(TEXT("Landing zone cells removed")));
			break;
		}
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
		case EOrderTool::RemoveFloor:
		{
			bool bFloor = false;
			for (int32 Y = Rect.minY; Y < Rect.minY + Rect.sizeY && !bFloor; ++Y)
			{
				for (int32 X = Rect.minX; X < Rect.minX + Rect.sizeX && !bFloor; ++X)
				{
					bFloor = Bridge->GetSim()->floors().at(colony::CellPos{X, Y, Rect.level}).valid();
				}
			}
			if (bFloor)
			{
				Submit(colony::DesignateCommand{Rect, colony::DesignationKind::RemoveFloor});
			}
			Say(bFloor ? FString(TEXT("Floor marked to be taken up")) : FString(TEXT("No floor there")));
			break;
		}
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
		case EOrderTool::LevelEarth:
		{
			const int32 HeightCm = LevelStartCm + LevelNudgeCm;
			Submit(colony::LevelEarthCommand{Rect, HeightCm});
			Say(FString::Printf(TEXT("Level earth %d x %d at %.2f m"), Rect.sizeX, Rect.sizeY, static_cast<float>(HeightCm) / 100.0f));
			break;
		}
		case EOrderTool::CancelEarth:
			Submit(colony::CancelEarthworksCommand{Rect});
			Say(TEXT("Earthworks cancelled"));
			break;
		case EOrderTool::PaintArea:
		case EOrderTool::EraseArea:
		{
			const bool bAdd = Tool == EOrderTool::PaintArea;
			Submit(colony::PaintAreaCommand{AreaToolArea, Rect, bAdd});
			Say(bAdd ? FString::Printf(TEXT("Area painted %d x %d"), Rect.sizeX, Rect.sizeY) : FString(TEXT("Area cells taken off")));
			break;
		}
		case EOrderTool::AnimalZone:
			Submit(colony::CreateAnimalZoneCommand{Rect});
			Say(FString::Printf(TEXT("Animal zone %d x %d"), Rect.sizeX, Rect.sizeY));
			break;
		case EOrderTool::Tame:
		case EOrderTool::Hunt:
		case EOrderTool::Slaughter:
		{
			// The sim marks every animal in the rectangle that can take the mark (wild ones to tame or hunt, tame ones to slaughter).
			const TCHAR* Mark = Tool == EOrderTool::Tame ? TEXT("Tame") : (Tool == EOrderTool::Hunt ? TEXT("Hunt") : TEXT("Slaughter"));
			Submit(colony::MarkAnimalsInAreaCommand{Rect, TCHAR_TO_UTF8(Mark), true});
			Say(FString::Printf(TEXT("%s: animals in %d x %d marked"), Mark, Rect.sizeX, Rect.sizeY));
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
		if (HudIcons::Button(HudIcons::Get(TEXT("pawn_undraft"), ImGui::GetFrameHeight()), TEXT("Stand down (R)"), false, ImVec2(0.0f, 0.0f)))
		{
			DraftSelected(false);
		}
		ImGui::TextUnformatted("Right click: move, or attack an enemy");
	}
	else if (!View.downed && HudIcons::Button(HudIcons::Get(TEXT("pawn_draft"), ImGui::GetFrameHeight()), TEXT("Draft (R)"), false, ImVec2(0.0f, 0.0f)))
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
		GrowZoneMaterial = Make(FLinearColor(OrdGrowColor.R, OrdGrowColor.G, OrdGrowColor.B, 0.22f));
		LandZoneMaterial = Make(FLinearColor(OrdLandColor.R, OrdLandColor.G, OrdLandColor.B, 0.16f));
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
		case OrdGrowTile:
			Box(FVector3f(96.0f, 96.0f, 3.0f), FVector3f::ZeroVector, OrdGrowColor);
			break;
		case OrdGrowRing:
			Ring(OrdGrowColor);
			break;
		case OrdLandTile:
			Box(FVector3f(96.0f, 96.0f, 3.0f), FVector3f::ZeroVector, OrdLandColor);
			break;
		case OrdLandRing:
			Ring(OrdLandColor);
			break;
		case OrdAreaRing:
			Ring(OrdAreaColor);
			break;
		case OrdAnimalRing:
			Ring(OrdAnimalColor);
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
	Stats.MineMarks = Stats.CutMarks = Stats.HarvestMarks = Stats.DeconstructMarks = Stats.ZoneCells = Stats.GrowCells = Stats.LandingCells = Stats.AnimalZoneCells = 0;

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
			case colony::DesignationKind::RemoveFloor:
				// A red plate on the floor to be taken up, like a building marked to come down.
				Place(OrdDeconstruct, 0, Mark.cell.x, Mark.cell.y, 3.0, nullptr);
				++Stats.DeconstructMarks;
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
	// Growing zones: a faint green tile under the crop, so a freshly drawn field shows before anything is sown.
	UMaterialInterface* GrowDraw = GrowZoneMaterial.Get();
	for (const colony::CellPos& Cell : Snapshot.growingZoneCells)
	{
		Place(GrowDraw ? OrdGrowTile : OrdGrowRing, 0, Cell.x, Cell.y, 1.0, GrowDraw);
		++Stats.GrowCells;
	}
	// The landing zone: a faint amber tile, a little above the others so it shows over a stockpile or field.
	UMaterialInterface* LandDraw = LandZoneMaterial.Get();
	for (const colony::CellPos& Cell : Snapshot.landingZone)
	{
		Place(LandDraw ? OrdLandTile : OrdLandRing, 0, Cell.x, Cell.y, 4.0, LandDraw);
		++Stats.LandingCells;
	}
	// Animal zones and pens (docs/design/husbandry.md): a tan ring on each cell, so a painted pen shows where its fence goes.
	for (const colony::CellPos& Cell : Snapshot.animalZoneCells)
	{
		Place(OrdAnimalRing, 0, Cell.x, Cell.y, 3.0, nullptr);
		++Stats.AnimalZoneCells;
	}

	// The allowed area being painted (docs/design/areas.md): a blue ring on each of its cells while an area tool is in hand.
	if (AreaToolArea != 0 && (Tool == EOrderTool::PaintArea || Tool == EOrderTool::EraseArea))
	{
		for (const colony::AreaView& Area : Snapshot.areas)
		{
			if (Area.id != AreaToolArea)
			{
				continue;
			}
			for (uint32 Index = Area.firstCell; Index < Area.firstCell + Area.cellCount && Index < Snapshot.areaCells.size(); ++Index)
			{
				const colony::CellPos& Cell = Snapshot.areaCells[Index];
				Place(OrdAreaRing, 0, Cell.x, Cell.y, 5.0, nullptr);
			}
		}
	}

	// Earthworks waiting (docs/design/earthworks.md): an amber ring on every cell round a corner still to be dug or filled.
	{
		const colony::Map& Map = Sim.map();
		TSet<int32> EarthCells;
		for (const uint32 Corner : Map.earthCorners())
		{
			const colony::CellPos K = Map.cellAt(static_cast<int>(Corner));
			for (int32 Y = K.y - 1; Y <= K.y; ++Y)
			{
				for (int32 X = K.x - 1; X <= K.x; ++X)
				{
					if (Map.inBounds(colony::CellPos{X, Y, 0}) && Sim.construction().siteAt(colony::CellPos{X, Y, 0}) == colony::kNoEntity)
					{
						EarthCells.Add(Map.index(colony::CellPos{X, Y, 0}));
					}
				}
			}
		}
		TArray<int32> Sorted = EarthCells.Array();
		Sorted.Sort();
		for (const int32 Index : Sorted)
		{
			const colony::CellPos Cell = Map.cellAt(Index);
			Place(OrdLandRing, 0, Cell.x, Cell.y, 3.0, nullptr);
		}
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
		for (const colony::CellPos& Cell : Snapshot.growingZoneCells)
		{
			Signature = OrdMix(Signature, OrdCellKey(Cell.x, Cell.y) ^ 0x67726F77ULL);
		}
		Signature = OrdMix(Signature, Snapshot.landingZoneChanges ^ 0x4C414E44ULL);
		for (const colony::CellPos& Cell : Snapshot.animalZoneCells)
		{
			Signature = OrdMix(Signature, OrdCellKey(Cell.x, Cell.y) ^ 0x414E494DULL);
		}
		if (AreaToolArea != 0 && (Tool == EOrderTool::PaintArea || Tool == EOrderTool::EraseArea))
		{
			Signature = OrdMix(Signature, (static_cast<uint64>(AreaToolArea) << 32) ^ 0x41524541ULL);
			for (const colony::AreaView& Area : Snapshot.areas)
			{
				Signature = OrdMix(Signature, Area.id == AreaToolArea ? static_cast<uint64>(Area.cellCount) : 0);
			}
		}
		Signature = OrdMix(Signature, Bridge->GetSim()->map().earthChanges() ^ 0x45415254ULL);
		Signature = OrdMix(Signature, static_cast<uint64>(Bridge->GetSim()->map().levelledCorners().size()));
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
			Signature = OrdMix(Signature, OrdCellKey(Origin.x, Origin.y) + (bValid ? 1 : 0) + (static_cast<uint64>(Origin.level) << 8));
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
		// Slate's own cursor first: the viewport's cached mouse position only moves while the viewport gets mouse moves.
		FVector2D Mouse;
		float MouseX = 0.0f;
		float MouseY = 0.0f;
		const APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
		bool bHaveMouse = CursorViewportPosition(Mouse);
		if (!bHaveMouse && PC && PC->GetMousePosition(MouseX, MouseY))
		{
			Mouse = FVector2D(MouseX, MouseY);
			bHaveMouse = true;
		}
		if (bHaveMouse)
		{
			const FSimPick Pick = PickGroundAt(Mouse);
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
	ColonyHud::ApplyFrameStyle(Scale);
	const float Margin = 12.0f * Scale;
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
	// Ahead of ImGui's input processor, which is up by now (see FOrdersClickProcessor).
	if (!bClickProcessorAtFront && FSlateApplication::IsInitialized())
	{
		if (ClickProcessor.IsValid())
		{
			FSlateApplication::Get().UnregisterInputPreProcessor(ClickProcessor);
		}
		ClickProcessor = MakeShared<FOrdersClickProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(ClickProcessor, 0);
		bClickProcessorAtFront = true;
	}
	// ImGui knows from the last frame whether the cursor is over one of its windows (or a popup is open). Not io.WantCaptureMouse:
	// that also stays on while ImGui thinks a mouse button is held.
	bMouseOverUi = CVarOrdersClickFirst.GetValueOnGameThread() == 0 ? ImGui::GetIO().WantCaptureMouse
		: ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
	if (ImGui::GetIO().WantCaptureMouse && !bMouseOverUi && !ImGui::IsAnyItemActive() && !bWarnedImGuiHeld)
	{
		bWarnedImGuiHeld = true;
		UE_LOG(LogOrders, Warning, TEXT("ImGui wants the mouse with no window under it (it thinks a button is held); world clicks with a tool go through anyway"));
	}
	ButtonRects.Reset();

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
		if (HudIcons::Button(HudIcons::Get(bAnyUndrafted ? TEXT("pawn_draft") : TEXT("pawn_undraft"), ImGui::GetFrameHeight()),
				bAnyUndrafted ? TEXT("ALARM##alarm") : TEXT("Stand down##alarm"), false, ImVec2(0.0f, 0.0f)) && bAnyColonist)
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

	// The Build menu (the bottom bar's Build tab), above the bar on the left. A message (what a drag did) shows in it, or on its
	// own above the bar while the menu is closed (building in direct mode).
	bBarShown = false;
	bBuildMenuShown = false;
	if (bBuildMenuOpen)
	{
		DrawBuildMenu(Bridge, Scale);
	}
	else if (!Message.IsEmpty() && FPlatformTime::Seconds() < MessageUntil)
	{
		ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, ColonyHud::AboveBar(Viewport, Scale)), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
		if (ImGui::Begin("##OrdersMessage", nullptr, ColonyHud::BoxFlags() | ImGuiWindowFlags_NoInputs))
		{
			OrdText(Message);
		}
		ImGui::End();
	}
}

void UOrdersSubsystem::SetBuildMenuOpen(bool bOpen)
{
	if (bBuildMenuOpen && !bOpen)
	{
		CancelTool();
	}
	bBuildMenuOpen = bOpen;
	if (bOpen && OpenCategory.IsEmpty())
	{
		OpenCategory = TEXT("Orders");
	}
}

// ---- Build tile pictures -----------------------------------------------------------------------

namespace
{
	TAutoConsoleVariable<int32> GOrdersTileIcons(TEXT("Orders.TileIcons"), 1,
		TEXT("1: Build tiles show the building's own mesh; 0: the drawn footprint icons."));
	TAutoConsoleVariable<int32> GOrdersTileIconsLit(TEXT("Orders.TileIconsLit"), 0,
		TEXT("1: tile pictures are lit by the world's light (shaded, but dark at night); 0: unlit (flat colours, the same at any hour)."));
	/** Where the pictures are taken: far under the map, where only the picture's own mesh is drawn (a show-only capture). */
	const FVector OrdIconStageAt(0.0, 0.0, -400000.0);
	constexpr int32 OrdIconPixels = 128;
}

UTexture* UOrdersSubsystem::GetTileIcon(colony::DefId Def)
{
	if (!Def.valid() || GOrdersTileIcons.GetValueOnGameThread() == 0)
	{
		return nullptr;
	}
	const int32 Mode = GOrdersTileIconsLit.GetValueOnGameThread() != 0 ? 1 : 0;
	if (Mode != TileIconMode)
	{
		TileIconMode = Mode;
		TileIconsReady.Reset();
		WantedTileIcons.Reset();
	}
	const int32 Key = static_cast<int32>(Def.value);
	if (TileIconsReady.Contains(Key))
	{
		const TObjectPtr<UTextureRenderTarget2D>* Found = TileIcons.Find(Key);
		return Found ? Found->Get() : nullptr;
	}
	if (!NoTileIcon.Contains(Key))
	{
		WantedTileIcons.AddUnique(Key);
	}
	return nullptr;
}

void UOrdersSubsystem::UpdateTileIcons()
{
	UWorld* World = GetWorld();
	UBuildingSubsystem* Buildings = World ? World->GetSubsystem<UBuildingSubsystem>() : nullptr;
	if (WantedTileIcons.IsEmpty() || !Buildings)
	{
		return;
	}
	const int32 Key = WantedTileIcons[0];
	WantedTileIcons.RemoveAt(0);
	colony::DefId Def;
	Def.value = static_cast<uint32>(Key);
	UStaticMesh* Mesh = Buildings->GetWholeMesh(Def, BestStuffFor(Def));
	if (!Mesh)
	{
		// The building subsystem may not be built yet: ask again later, unless the def has no picture at all.
		if (Buildings->IsBuilt() && Buildings->GetDrawnHeightCm(Def) <= 0.0f)
		{
			NoTileIcon.Add(Key);
		}
		return;
	}
	if (!IconStage)
	{
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		IconStage = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(OrdIconStageAt), Params);
		if (!IconStage)
		{
			return;
		}
		USceneComponent* Root = NewObject<USceneComponent>(IconStage);
		IconStage->SetRootComponent(Root);
		Root->RegisterComponent();
		IconStage->SetActorLocation(OrdIconStageAt);
		IconMesh = NewObject<UStaticMeshComponent>(IconStage);
		IconMesh->SetupAttachment(Root);
		IconMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		IconMesh->SetCastShadow(false);
		IconMesh->RegisterComponent();
		IconCapture = NewObject<USceneCaptureComponent2D>(IconStage);
		IconCapture->SetupAttachment(Root);
		IconCapture->RegisterComponent();
		IconCapture->bCaptureEveryFrame = false;
		IconCapture->bCaptureOnMovement = false;
		IconCapture->FOVAngle = 30.0f;
		IconCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		IconCapture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
		IconCapture->ShowFlags.SetAtmosphere(false);
		IconCapture->ShowFlags.SetFog(false);
		IconCapture->ShowFlags.SetMotionBlur(false);
		IconCapture->ShowFlags.SetEyeAdaptation(false);
		IconCapture->ShowOnlyComponents.Add(IconMesh);
	}
	IconCapture->ShowFlags.SetLighting(GOrdersTileIconsLit.GetValueOnGameThread() != 0);
	IconMesh->SetStaticMesh(Mesh);
	IconMesh->SetWorldLocation(OrdIconStageAt);
	// From the south-west, above at about 35 degrees, far enough back that the whole mesh fits.
	const FBoxSphereBounds Bounds = IconMesh->CalcBounds(IconMesh->GetComponentTransform());
	const float Radius = FMath::Max(10.0f, static_cast<float>(Bounds.SphereRadius));
	const float Distance = Radius / FMath::Tan(FMath::DegreesToRadians(IconCapture->FOVAngle * 0.5f)) * 1.05f;
	const FVector Look = FVector(1.0, 0.8, -0.85).GetSafeNormal();
	const FVector Eye = Bounds.Origin - Look * Distance;
	IconCapture->SetWorldLocationAndRotation(Eye, Look.Rotation());

	TObjectPtr<UTextureRenderTarget2D>& Target = TileIcons.FindOrAdd(Key);
	if (!Target)
	{
		Target = NewObject<UTextureRenderTarget2D>(this);
		Target->RenderTargetFormat = RTF_RGBA8;
		// The tile's own colour, so the picture sits in the button with no box round it.
		const ImVec4 Back = ColonyHud::ButtonBg();
		Target->ClearColor = FLinearColor(Back.x, Back.y, Back.z, 1.0f);
		Target->InitAutoFormat(OrdIconPixels, OrdIconPixels);
		Target->UpdateResourceImmediate(true);
	}
	IconCapture->TextureTarget = Target;
	IconCapture->CaptureScene();
	TileIconsReady.Add(Key);
	++Stats.TileIconsMade;
}

void UOrdersSubsystem::DrawBuildMenu(USimSubsystem& Bridge, float Scale)
{
	const ImGuiViewport* Viewport = ImGui::GetMainViewport();
	const colony::Sim& Sim = *Bridge.GetSim();
	const float Margin = 12.0f * Scale;
	const float Bottom = ColonyHud::AboveBar(Viewport, Scale);

	// Research finished since the menu was read: locked entries open up.
	int32 Done = 0;
	for (const colony::ResearchView& Project : Bridge.GetCurrentSnapshot().research)
	{
		Done += Project.done ? 1 : 0;
	}
	if (Done != ResearchDoneSeen)
	{
		ReadBuildOptions();
	}

	const auto Record = [this](const FString& Label)
	{
		const ImVec2 Min = ImGui::GetItemRectMin();
		const ImVec2 Max = ImGui::GetItemRectMax();
		ButtonRects.Add(Label, FBox2D(FVector2D(Min.x, Min.y), FVector2D(Max.x, Max.y)));
	};
	FBox2D Menu(ForceInit);
	const auto Grow = [&Menu]()
	{
		const ImVec2 Pos = ImGui::GetWindowPos();
		const ImVec2 Size = ImGui::GetWindowSize();
		Menu += FBox2D(FVector2D(Pos.x, Pos.y), FVector2D(Pos.x + Size.x, Pos.y + Size.y));
	};

	// Left: the search box and the categories, two columns, the picked one lit.
	const float NavWidth = 250.0f * Scale;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Margin, Bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(NavWidth, 0.0f), ImGuiCond_Always);
	if (ImGui::Begin("##BuildNav", nullptr, ColonyHud::BoxFlags()))
	{
		ImGui::SetNextItemWidth(-1.0f);
		ImGui::InputTextWithHint("##BuildSearch", "Search", BuildSearch, sizeof(BuildSearch));
		TArray<FString> Categories = {TEXT("Orders"), TEXT("Zone")};
		Categories.Append(BuildCategories);
		// Only the categories there are (an odd count leaves the last cell of the grid empty, not drawn).
		const int32 Slots = Categories.Num();
		const float CellWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		for (int32 Index = 0; Index < Slots; ++Index)
		{
			if (Index % 2 == 1)
			{
				ImGui::SameLine();
			}
			if (Index >= Categories.Num())
			{
				// Spare slots for categories still to come.
				ImGui::BeginDisabled();
				ImGui::PushID(Index);
				ImGui::Button("##spare", ImVec2(CellWidth, 0.0f));
				ImGui::PopID();
				ImGui::EndDisabled();
				continue;
			}
			const FString& Name = Categories[Index];
			if (HudIcons::Button(HudIcons::Get(OrdCategoryIcon(Name), ImGui::GetFrameHeight()), Name, OpenCategory == Name && BuildSearch[0] == 0, ImVec2(CellWidth, 0.0f)))
			{
				OpenCategory = Name;
				BuildSearch[0] = 0;
			}
			Record(Name);
		}
		Grow();
	}
	ImGui::End();

	// The tiles of the open category (or what the search finds): Cancel and Deconstruct first.
	struct FTile
	{
		FString Label;
		FString Tip;
		EOrderTool TileTool = EOrderTool::None;
		const FBuildOption* Option = nullptr;
	};
	TArray<FTile> Tiles;
	const auto AddTool = [&Tiles](const TCHAR* Label, EOrderTool ForTool, const TCHAR* Tip)
	{
		FTile Tile;
		Tile.Label = Label;
		Tile.TileTool = ForTool;
		Tile.Tip = Tip;
		Tiles.Add(Tile);
	};
	const FString Search = UTF8_TO_TCHAR(BuildSearch);
	const auto AddOption = [&Tiles, &Sim](const FBuildOption& Option)
	{
		FTile Tile;
		Tile.Label = Option.Label;
		Tile.Option = &Option;
		const std::string About = Sim.defs().get(Option.Def).text("description");
		Tile.Tip = Option.Locked.IsEmpty() ? FString(UTF8_TO_TCHAR(About.c_str())) : FString::Printf(TEXT("Needs research: %s"), *Option.Locked);
		Tiles.Add(Tile);
	};
	if (!Search.IsEmpty())
	{
		for (const FBuildOption& Option : BuildOptions)
		{
			if (Option.Label.Contains(Search))
			{
				AddOption(Option);
			}
		}
	}
	else if (OpenCategory == TEXT("Orders"))
	{
		AddTool(TEXT("Cancel"), EOrderTool::Cancel, TEXT("Drag over marks, plans and frames to take them off"));
		AddTool(TEXT("Deconstruct"), EOrderTool::Deconstruct, TEXT("Drag over buildings to have them taken down"));
		AddTool(TEXT("Mine"), EOrderTool::Mine, TEXT("Drag over rock to have it mined"));
		AddTool(TEXT("Chop trees"), EOrderTool::Chop, TEXT("Drag over trees and plants to have them cut"));
		AddTool(TEXT("Harvest"), EOrderTool::Harvest, TEXT("Drag over grown plants to have them harvested"));
		AddTool(TEXT("Remove floor"), EOrderTool::RemoveFloor, TEXT("Drag over laid floors to have them taken up"));
		AddTool(TEXT("Tame"), EOrderTool::Tame, TEXT("Drag over wild animals: a colonist with Handling work tries to tame them"));
		AddTool(TEXT("Hunt"), EOrderTool::Hunt, TEXT("Drag over wild animals: a colonist with Hunting work and a gun shoots them for meat"));
		AddTool(TEXT("Slaughter"), EOrderTool::Slaughter, TEXT("Drag over tame animals: a colonist with Handling work kills them for meat"));
	}
	else if (OpenCategory == TEXT("Zone"))
	{
		AddTool(TEXT("Cancel"), EOrderTool::Cancel, TEXT("Drag over marks, plans and frames to take them off"));
		AddTool(TEXT("Stockpile"), EOrderTool::Stockpile, TEXT("Drag a stockpile zone: haulers bring things there"));
		AddTool(TEXT("Growing zone"), EOrderTool::Grow, TEXT("Drag a growing zone on soil: growers sow the crop you pick"));
		AddTool(TEXT("Animal zone"), EOrderTool::AnimalZone, TEXT("Drag an animal zone; pick it for animals in the Animals tab. Ring it with fence and a gate to make a pen"));
		AddTool(TEXT("Remove zone"), EOrderTool::RemoveZone, TEXT("Drag over zone cells to take them out of their zones"));
		AddTool(TEXT("Landing zone"), EOrderTool::LandingZone, TEXT("Drag where shuttles may set down"));
		AddTool(TEXT("Remove landing zone"), EOrderTool::RemoveLandingZone, TEXT("Drag over landing zone cells to take them out"));
	}
	else if (!OpenCategory.IsEmpty())
	{
		AddTool(TEXT("Cancel"), EOrderTool::Cancel, TEXT("Drag over marks, plans and frames to take them off"));
		AddTool(TEXT("Deconstruct"), EOrderTool::Deconstruct, TEXT("Drag over buildings to have them taken down"));
		for (const FBuildOption& Option : BuildOptions)
		{
			if (Option.Category == OpenCategory)
			{
				AddOption(Option);
			}
		}
		if (OpenCategory == TEXT("Structure"))
		{
			AddTool(TEXT("Level earth"), EOrderTool::LevelEarth, TEXT("Drag to have the ground dug or filled flat"));
			AddTool(TEXT("Cancel earthworks"), EOrderTool::CancelEarth, TEXT("Drag over planned earthworks to drop them"));
		}
	}

	const auto Activate = [this](const FTile& Tile)
	{
		if (Tile.Option)
		{
			if (Tool == EOrderTool::Build && BuildDef == Tile.Option->Def)
			{
				CancelTool();
			}
			else
			{
				SelectBuilding(Tile.Option->DefName);
			}
		}
		else
		{
			SetTool(Tool == Tile.TileTool ? EOrderTool::None : Tile.TileTool);
		}
	};

	// Hotkeys: the letter in each tile's corner, while nothing is being typed.
	if (!ImGui::GetIO().WantTextInput)
	{
		if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
		{
			const int32 KeyCount = FCString::Strlen(OrdTileKeys);
			for (int32 Index = 0; Index < Tiles.Num() && Index < KeyCount; ++Index)
			{
				const TCHAR Letter[2] = {OrdTileKeys[Index], 0};
				if (PC->WasInputKeyJustPressed(FKey(FName(Letter))))
				{
					Activate(Tiles[Index]);
					break;
				}
			}
		}
	}

	float TilesTop = Bottom;
	const float TilesLeft = Viewport->Pos.x + Margin + NavWidth + 8.0f * Scale;
	if (!Tiles.IsEmpty())
	{
		// Tiles are a little taller than wide: the picture on top, the name (up to two lines) under it at a readable size.
		const float TileSize = 92.0f * Scale;
		const float TileHeight = TileSize * 1.2f;
		const float Step = TileSize + ImGui::GetStyle().ItemSpacing.x;
		const float Pad = 2.0f * ImGui::GetStyle().WindowPadding.x;
		float MaxWidth = Viewport->Size.x - (TilesLeft - Viewport->Pos.x) - 330.0f * Scale;  // the date box stays clear
		if (MaxWidth < 4.0f * Step + Pad)
		{
			// A narrow window: the tiles may cover the date box rather than stand in a tall column.
			MaxWidth = Viewport->Size.x - (TilesLeft - Viewport->Pos.x) - Margin;
		}
		// As many a row as fit, then evened out so the last row isn't one lone tile (27 tiles at 13 a row: 9, 9, 9).
		const int32 Fit = FMath::Max(1, static_cast<int32>((MaxWidth - Pad) / Step));
		const int32 RowCount = FMath::DivideAndRoundUp(Tiles.Num(), Fit);
		const int32 PerRow = FMath::Max(1, FMath::DivideAndRoundUp(Tiles.Num(), RowCount));
		ImGui::SetNextWindowPos(ImVec2(TilesLeft, Bottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		// Never taller than the room above the bar (the rest scrolls), so the window stays on screen.
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, FMath::Max(120.0f * Scale, Bottom - Viewport->Pos.y - 150.0f * Scale)));
		if (ImGui::Begin("##BuildTiles", nullptr, ColonyHud::BoxFlags()))
		{
			ImDrawList* Draw = ImGui::GetWindowDrawList();
			for (int32 Index = 0; Index < Tiles.Num(); ++Index)
			{
				const FTile& Tile = Tiles[Index];
				if (Index % PerRow != 0)
				{
					ImGui::SameLine();
				}
				const bool bLocked = Tile.Option && !Tile.Option->Locked.IsEmpty();
				const bool bActive = Tile.Option ? (Tool == EOrderTool::Build && BuildDef == Tile.Option->Def) : (Tool == Tile.TileTool && Tool != EOrderTool::None);
				ImGui::PushID(Index);
				const ImVec2 Min = ImGui::GetCursorScreenPos();
				if (bLocked)
				{
					ImGui::PushStyleColor(ImGuiCol_Text, ColonyHud::TextFaint());
					ImGui::PushStyleColor(ImGuiCol_Button, ColonyHud::Locked());
				}
				// The button carries no text: the name is drawn under the picture below, on two lines when it is long.
				bool bClicked = false;
				if (bActive)
				{
					bClicked = ColonyHud::Button(TEXT("##tile"), true, ImVec2(TileSize, TileHeight));
				}
				else
				{
					bClicked = ImGui::Button("##tile", ImVec2(TileSize, TileHeight));
				}
				{
					// A long name wraps word by word at 90% of the text size (up to three lines), and shrinks only if a single word
					// is still too wide ("Ground-penetrating").
					const float Room = TileSize - 2.0f * ImGui::GetStyle().FramePadding.x;
					const float WrapScale = ImGui::CalcTextSize(TCHAR_TO_UTF8(*Tile.Label)).x > Room ? 0.9f : 1.0f;
					const auto WidthAt = [](const FString& Text, float Size) { return ImGui::GetFont()->CalcTextSizeA(Size, FLT_MAX, 0.0f, TCHAR_TO_UTF8(*Text)).x; };
					const float WrapSize = ImGui::GetFontSize() * WrapScale;
					TArray<FString> Words;
					Tile.Label.ParseIntoArray(Words, TEXT(" "));
					TArray<FString> Lines;
					for (const FString& Word : Words)
					{
						if (Lines.Num() > 0 && Lines.Num() <= 3 && WidthAt(Lines.Last() + TEXT(" ") + Word, WrapSize) <= Room)
						{
							Lines.Last() += TEXT(" ") + Word;
						}
						else if (Lines.Num() < 3)
						{
							Lines.Add(Word);
						}
						else
						{
							Lines.Last() += TEXT(" ") + Word;
						}
					}
					if (Lines.IsEmpty())
					{
						Lines.Add(Tile.Label);
					}
					float Widest = 1.0f;
					for (const FString& Line : Lines)
					{
						Widest = FMath::Max(Widest, WidthAt(Line, WrapSize));
					}
					// Never taller than the room under the picture (which ends at 68% of the tile's width), so three lines don't
					// run into it.
					const float NameRoom = TileHeight - 5.0f * Scale - TileSize * 0.70f;
					const float FontSize = FMath::Min(WrapSize * FMath::Clamp(Room / Widest, 0.75f, 1.0f), NameRoom / static_cast<float>(Lines.Num()));
					const ImU32 NameColor = ImGui::GetColorU32(bLocked ? ColonyHud::TextFaint() : ColonyHud::TextMain());
					float LineY = Min.y + TileHeight - 5.0f * Scale - FontSize * Lines.Num();
					for (const FString& Line : Lines)
					{
						const FTCHARToUTF8 Utf8(*Line);
						const float LineWidth = ImGui::GetFont()->CalcTextSizeA(FontSize, FLT_MAX, 0.0f, Utf8.Get()).x;
						Draw->AddText(ImGui::GetFont(), FontSize, ImVec2(Min.x + (TileSize - LineWidth) * 0.5f, LineY), NameColor, Utf8.Get());
						LineY += FontSize;
					}
				}
				if (bLocked)
				{
					ImGui::PopStyleColor(2);
				}
				Record(Tile.Label);
				if (bClicked && !bLocked)
				{
					Activate(Tile);
				}
				else if (bClicked)
				{
					Say(Tile.Tip);
				}
				ColonyHud::Tip(Tile.Tip);
				// The hotkey in the corner, and an icon: the footprint for a building, a hatched square for a floor, the tool's colour for an order.
				if (Index < FCString::Strlen(OrdTileKeys))
				{
					const char Key[2] = {static_cast<char>(OrdTileKeys[Index]), 0};
					Draw->AddText(ImVec2(Min.x + 5.0f * Scale, Min.y + 3.0f * Scale), ImGui::GetColorU32(ColonyHud::TextFaint()), Key);
				}
				const ImVec2 IconCentre(Min.x + TileSize * 0.5f, Min.y + TileSize * 0.40f);
				const float IconSize = TileSize * 0.40f;
				const ImU32 IconColor = ImGui::GetColorU32(bLocked ? ColonyHud::TextFaint() : ColonyHud::Rgb(0xd6d0c3));
				if (Tile.Option && Tile.Option->bFloor)
				{
					const ImVec2 A(IconCentre.x - IconSize * 0.5f, IconCentre.y - IconSize * 0.5f);
					const ImVec2 B(IconCentre.x + IconSize * 0.5f, IconCentre.y + IconSize * 0.5f);
					Draw->AddRect(A, B, IconColor, 2.0f, 1.5f, 0);
					for (int32 Line = 1; Line < 4; ++Line)
					{
						const float T = IconSize * Line / 4.0f;
						Draw->AddLine(ImVec2(A.x + T, A.y), ImVec2(A.x, A.y + T), IconColor, 1.0f);
						Draw->AddLine(ImVec2(B.x - T, B.y), ImVec2(B.x, B.y - T), IconColor, 1.0f);
					}
				}
				else if (UTexture* Picture = Tile.Option ? GetTileIcon(Tile.Option->Def) : nullptr)
				{
					const float Half = TileSize * 0.28f;
					Draw->AddImage(ImTextureRef(Picture), ImVec2(IconCentre.x - Half, IconCentre.y - Half), ImVec2(IconCentre.x + Half, IconCentre.y + Half),
						ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), bLocked ? IM_COL32(255, 255, 255, 90) : IM_COL32(255, 255, 255, 255));
				}
				else if (Tile.Option)
				{
					const int32 W = FMath::Clamp(Tile.Option->SizeX, 1, 4);
					const int32 H = FMath::Clamp(Tile.Option->SizeY, 1, 4);
					const float Cell = IconSize / FMath::Max(W, H);
					const ImVec2 A(IconCentre.x - Cell * W * 0.5f, IconCentre.y - Cell * H * 0.5f);
					for (int32 Y = 0; Y < H; ++Y)
					{
						for (int32 X = 0; X < W; ++X)
						{
							Draw->AddRectFilled(ImVec2(A.x + X * Cell + 1.0f, A.y + Y * Cell + 1.0f), ImVec2(A.x + (X + 1) * Cell - 1.0f, A.y + (Y + 1) * Cell - 1.0f), IconColor, 2.0f);
						}
					}
				}
				else if (UTexture* ToolIcon = HudIcons::Get(OrdToolIcon(Tile.TileTool), TileSize * 0.62f))
				{
					HudIcons::Draw(Draw, ToolIcon, ImVec2(IconCentre.x, IconCentre.y + TileSize * 0.02f), TileSize * 0.62f, bLocked);
				}
				else
				{
					const FColor Color = OrdToolColor(Tile.TileTool);
					Draw->AddCircle(IconCentre, IconSize * 0.45f, IM_COL32(Color.R, Color.G, Color.B, 255), 24, 3.0f * Scale);
					if (Tile.TileTool == EOrderTool::Cancel || Tile.TileTool == EOrderTool::Deconstruct || Tile.TileTool == EOrderTool::RemoveZone ||
						Tile.TileTool == EOrderTool::RemoveFloor || Tile.TileTool == EOrderTool::RemoveLandingZone || Tile.TileTool == EOrderTool::CancelEarth)
					{
						const float R = IconSize * 0.3f;
						Draw->AddLine(ImVec2(IconCentre.x - R, IconCentre.y - R), ImVec2(IconCentre.x + R, IconCentre.y + R), IM_COL32(Color.R, Color.G, Color.B, 255), 3.0f * Scale);
					}
				}
				ImGui::PopID();
			}
			Grow();
			TilesTop = ImGui::GetWindowPos().y;
			UpdateTileIcons();
		}
		ImGui::End();
	}

	// The strip about the tool in hand: name, what it does, material, drag shape and what the drag plans and costs.
	const bool bMessage = !Message.IsEmpty() && FPlatformTime::Seconds() < MessageUntil;
	if (Tool != EOrderTool::None || bMessage)
	{
		ImGui::SetNextWindowPos(ImVec2(TilesLeft, TilesTop - 6.0f * Scale), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
		ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(Viewport->Size.x - (TilesLeft - Viewport->Pos.x) - Margin, FLT_MAX));
		if (ImGui::Begin("##BuildStrip", nullptr, ColonyHud::BoxFlags()))
		{
			if (Tool == EOrderTool::Build && BuildDef.valid())
			{
				const colony::Def& Def = Sim.defs().get(BuildDef);
				ImGui::BeginGroup();
				ColonyHud::Title(OrdCapital(Def.label()));
				FString About = UTF8_TO_TCHAR(Def.text("description").c_str());
				int32 Stop = INDEX_NONE;
				if (About.FindChar(TEXT('.'), Stop) && Stop > 0)
				{
					About = About.Left(Stop + 1);
				}
				ColonyHud::TextColored(ColonyHud::TextDim(), About.Len() > 96 ? About.Left(93) + TEXT("...") : About);
				ImGui::EndGroup();
				// Material, with how much lies on the map.
				if (!StuffOptions.IsEmpty())
				{
					ImGui::SameLine(0.0f, 16.0f * Scale);
					ImGui::BeginGroup();
					ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Made of"));
					for (int32 Index = 0; Index < StuffOptions.Num() && Index < 6; ++Index)
					{
						const FStuffOption& Option = StuffOptions[Index];
						if (Index > 0)
						{
							ImGui::SameLine();
						}
						if (ColonyHud::Button(FString::Printf(TEXT("%s %d##stuff%d"), *Option.Label, Option.InStock, Index), Option.Def == BuildStuff))
						{
							BuildStuff = Option.Def;
						}
					}
					ImGui::EndGroup();
				}
				// The drag shape.
				ImGui::SameLine(0.0f, 16.0f * Scale);
				ImGui::BeginGroup();
				ColonyHud::TextColored(ColonyHud::TextFaint(), TEXT("Drag"));
				const colony::DragShape Shapes[] = {colony::DragShape::Single, colony::DragShape::Line, colony::DragShape::Outline, colony::DragShape::Filled};
				const TCHAR* const ShapeTips[] = {TEXT("One, where you let go"), TEXT("A row along the drag"), TEXT("The edge of the dragged rectangle: a room's walls"),
					TEXT("Every cell of the dragged rectangle")};
				for (int32 Index = 0; Index < 4; ++Index)
				{
					if (Index > 0)
					{
						ImGui::SameLine();
					}
					if (ColonyHud::Button(UTF8_TO_TCHAR(colony::dragShapeName(Shapes[Index])), BuildShape == Shapes[Index]))
					{
						BuildShape = Shapes[Index];
					}
					ColonyHud::Tip(ShapeTips[Index]);
				}
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("Turn (R)")))
				{
					RotateBuilding();
				}
				ImGui::EndGroup();

				// What the drag plans and costs, worked out again only when it changes.
				if (bHasHover)
				{
					const colony::CellPos From = bDragging ? DragStart : Hover;
					TArray<colony::CellPos> Origins;
					GetBuildOrigins(Origins);
					uint64 Signature = OrdMix(OrdMix(OrdMix(OrdMix(7, BuildDef.value), BuildStuff.value), static_cast<uint64>(BuildRot)), static_cast<uint64>(BuildShape));
					Signature = OrdMix(OrdMix(Signature, OrdCellKey(From.x, From.y)), OrdCellKey(Hover.x, Hover.y) + static_cast<uint64>(Hover.level));
					Signature = OrdMix(Signature, static_cast<uint64>(Bridge.GetCurrentSnapshot().gameSeconds / 10));
					if (Signature != PlanSignature && !Origins.IsEmpty())
					{
						PlanSignature = Signature;
						const colony::CellPos Start = BuildShape == colony::DragShape::Single ? Origins[0] : From;
						const colony::CellPos End = BuildShape == colony::DragShape::Single ? Origins[0] : Hover;
						const colony::BuildPreview Preview = colony::BuildPlanner(Sim).preview(Def.defName(),
							BuildStuff.valid() ? Sim.defs().get(BuildStuff).defName() : std::string(), Start, End, BuildRot, BuildShape);
						if (!Preview.whyNot.empty())
						{
							PlanText = UTF8_TO_TCHAR(Preview.whyNot.c_str());
							bPlanAffordable = false;
						}
						else
						{
							FString Cost;
							for (const colony::BuildCostLine& Line : Preview.cost)
							{
								Cost += FString::Printf(TEXT("%s%d %s"), Cost.IsEmpty() ? TEXT("") : TEXT(", "), Line.needed, UTF8_TO_TCHAR(Line.label.c_str()));
							}
							PlanText = FString::Printf(TEXT("%d planned%s%s"), Preview.plannable, Cost.IsEmpty() ? TEXT("") : TEXT("  -  "), *Cost);
							bPlanAffordable = Preview.affordable();
						}
					}
					ImGui::SameLine(0.0f, 16.0f * Scale);
					ColonyHud::TextColored(bPlanAffordable ? ColonyHud::Good() : ColonyHud::Warn(), PlanText);
					if (!bPlanAffordable)
					{
						ColonyHud::Tip(TEXT("Not enough on the map yet: builders wait for more, as in RimWorld"));
					}
				}
			}
			else if (Tool == EOrderTool::Grow)
			{
				if (!GrowCrop.valid())
				{
					SetGrowCrop(TEXT("Plant_Rice"));
				}
				// The crops the colony may sow now (research done), with their growing time.
				ColonyHud::Title(TEXT("Growing zone"));
				const FString Preview = GrowCrop.valid() ? OrdCapital(Sim.defs().get(GrowCrop).label()) : FString(TEXT("(none)"));
				OrdText(TEXT("Crop"));
				ImGui::SameLine();
				ImGui::SetNextItemWidth(220.0f * Scale);
				if (ImGui::BeginCombo("##OrdersCrop", TCHAR_TO_UTF8(*Preview)))
				{
					for (const colony::DefId Crop : Sim.farming().crops())
					{
						const colony::Def& Def = Sim.defs().get(Crop);
						if (!Sim.research().unlocked(Def))
						{
							continue;
						}
						// Days to grow in good soil: growDays round the clock, 13 growing hours a day, times the life pace.
						const double Days = Def.number("plant.growDays", 1.0) * 24.0 / 13.0 / FMath::Max(Sim.plants().growthTuning(), 0.01);
						const FString Label = FString::Printf(TEXT("%s (%.1f days)"), *OrdCapital(Def.label()), Days);
						if (ImGui::Selectable(TCHAR_TO_UTF8(*Label), Crop == GrowCrop))
						{
							GrowCrop = Crop;
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Drag a rectangle on open soil. Colonists with Growing work sow it."));
			}
			else if (Tool == EOrderTool::LevelEarth)
			{
				// The height comes from the ground where the drag starts; these nudge it in 25 cm steps (at most twice a wall's
				// height from any corner: the sim leaves out cells further off).
				ColonyHud::Title(TEXT("Level earth"));
				const int32 StartCm = bDragging ? LevelStartCm : (bHasHover ? LevelHeightAtCm(Hover) : 0);
				OrdText(FString::Printf(TEXT("Height %.2f m"), static_cast<float>(StartCm + LevelNudgeCm) / 100.0f));
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("Lower 25 cm")))
				{
					LevelNudgeCm = FMath::Max(LevelNudgeCm - 25, -600);
				}
				ImGui::SameLine();
				if (ColonyHud::Button(TEXT("Raise 25 cm")))
				{
					LevelNudgeCm = FMath::Min(LevelNudgeCm + 25, 600);
				}
				if (LevelNudgeCm != 0)
				{
					ImGui::SameLine();
					if (ColonyHud::Button(TEXT("Reset")))
					{
						LevelNudgeCm = 0;
					}
				}
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Drag from the height you want. Builders dig or fill it, about a second per quarter metre."));
			}
			else if (Tool == EOrderTool::AnimalZone)
			{
				ColonyHud::Title(TEXT("Animal zone"));
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Drag rectangles. Then pick the zone for animals in the Animals tab. Ring it with fence (Structure) and a fence gate to make a pen they can't leave."));
			}
			else if (Tool == EOrderTool::LandingZone)
			{
				ColonyHud::Title(TEXT("Landing zone"));
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Drag rectangles on open ground. Shuttles set down inside it when a whole 6 x 12 shuttle fits."));
			}
			else if (Tool != EOrderTool::None)
			{
				ColonyHud::Title(OrdToolLabel(Tool));
				ColonyHud::TextColored(ColonyHud::TextDim(), TEXT("Drag a rectangle on the ground. Right click or Esc puts it down."));
			}
			if (bMessage)
			{
				ColonyHud::TextColored(ColonyHud::Accent(), Message);
			}
			Grow();
		}
		ImGui::End();
	}
	BuildMenuRect = Menu;
	bBuildMenuShown = Menu.bIsValid;
	BarRect = Menu;
	bBarShown = Menu.bIsValid;
}

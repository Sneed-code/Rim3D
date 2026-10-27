#include "BuildingSubsystem.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "LevelsSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include <string>

DEFINE_LOG_CATEGORY_STATIC(LogSimBuildings, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GBuildingsEnable(
		TEXT("Buildings.Enable"), 1, TEXT("1 draws the sim's buildings, blueprints and items, 0 draws nothing."), ECVF_Default);

	// The same flat-colour material as the terrain, and the see-through one the BuildingMaterials commandlet makes for the ghosts.
	const TCHAR* const BldSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const BldGhostMaterialPath = TEXT("/Game/Rim3D/Generated/Buildings/M_BuildingGhost.M_BuildingGhost");
	const FLinearColor BldGhostColor(0.18f, 0.42f, 0.95f, 0.38f);

	// What a component draws: the fixed parts of a finished building, a door's leaf, a frame (all parts, solid), a ghost (all parts, see-through).
	constexpr int32 BldFixed = 0;
	constexpr int32 BldSliding = 1;
	constexpr int32 BldAll = 2;
	constexpr int32 BldGhost = 3;

	// A frame is drawn this fraction of its height with nothing delivered, and a full height when it is done.
	constexpr float BldFrameMinScale = 0.1f;
	// Frame heights come in this many steps, so a chunk is not rebuilt for every second of work.
	constexpr int32 BldFrameSteps = 10;
	// Items are not drawn beyond this (cm), starting to vanish at 80% of it.
	constexpr float BldItemCullCm = 15000.0f;

	uint64 BldMix(uint64 Value)
	{
		Value += 0x9E3779B97F4A7C15ULL;
		Value = (Value ^ (Value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		Value = (Value ^ (Value >> 27)) * 0x94D049BB133111EBULL;
		return Value ^ (Value >> 31);
	}

	uint64 BldCombine(uint64 Seed, uint64 Value)
	{
		return BldMix(Seed * 0x100000001B3ULL ^ Value);
	}

	float BldYawOf(colony::Rot4 Rot)
	{
		switch (Rot)
		{
			case colony::Rot4::East: return 90.0f;
			case colony::Rot4::South: return 180.0f;
			case colony::Rot4::West: return 270.0f;
			default: return 0.0f;
		}
	}

	int32 BldChunkOf(float Metres)
	{
		return FMath::FloorToInt32(Metres / static_cast<float>(TerrainDraw::ChunkCells));
	}

	/** How many pieces of an item's pile a stack of `Count` is drawn as: one for the smallest, all of them for a full stack. */
	int32 BldPieces(int32 MaxPieces, int32 StackLimit, int32 Count)
	{
		if (MaxPieces <= 1 || StackLimit <= 1)
		{
			return 1;
		}
		const float Fill = FMath::Clamp(static_cast<float>(Count) / static_cast<float>(StackLimit), 0.0f, 1.0f);
		return FMath::Clamp(FMath::CeilToInt32(Fill * static_cast<float>(MaxPieces)), 1, MaxPieces);
	}

	/** A frame's height as a fraction of the finished building's: from the materials delivered, then the work done. */
	float BldFrameScale(const colony::ThingView& View)
	{
		const float Materials = View.materialsNeeded > 0 ? FMath::Clamp(static_cast<float>(View.materialsHave) / static_cast<float>(View.materialsNeeded), 0.0f, 1.0f) : 1.0f;
		const float Done = BldFrameMinScale + (1.0f - BldFrameMinScale) * (0.25f * Materials + 0.75f * FMath::Clamp(View.buildProgress, 0.0f, 1.0f));
		return static_cast<float>(FMath::FloorToInt32(Done * BldFrameSteps)) / static_cast<float>(BldFrameSteps);
	}

	/** Whether a door's leaf runs along y: the walls it sits in are to its north and south. Otherwise the door's facing says. */
	bool BldDoorAlongY(const colony::Map& Map, const colony::ThingView& View)
	{
		const int32 X = FMath::FloorToInt32(View.x);
		const int32 Y = FMath::FloorToInt32(View.y);
		const auto Wall = [&Map, &View](int32 CX, int32 CY)
		{
			const colony::CellPos Cell{CX, CY, View.level};
			return Map.inBounds(Cell) && Map.passage(Cell) == colony::Passage::Blocked;
		};
		const bool bWallsEastWest = Wall(X - 1, Y) || Wall(X + 1, Y);
		const bool bWallsNorthSouth = Wall(X, Y - 1) || Wall(X, Y + 1);
		if (bWallsEastWest != bWallsNorthSouth)
		{
			return bWallsNorthSouth;
		}
		return View.rotation == colony::Rot4::East || View.rotation == colony::Rot4::West;
	}

	struct FBldBatch
	{
		UStaticMesh* Mesh = nullptr;
		bool bGhost = false;
		bool bItem = false;
		bool bShadow = true;
		TArray<FTransform> Transforms;
	};
}

bool UBuildingSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UBuildingSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UBuildingSubsystem, STATGROUP_Tickables);
}

void UBuildingSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Bridge = Collection.InitializeDependency<USimSubsystem>();
}

void UBuildingSubsystem::Deinitialize()
{
	// The world is going away and takes the actor and its components with it.
	DrawActor = nullptr;
	Chunks.Reset();
	DoorSlots.Reset();
	Super::Deinitialize();
}

void UBuildingSubsystem::Teardown()
{
	if (DrawActor)
	{
		DrawActor->Destroy();
	}
	DrawActor = nullptr;
	Chunks.Reset();
	DoorSlots.Reset();
	Looks.Reset();
	LookIndex.Reset();
	DefLooks.Reset();
	KeepMeshes.Reset();
	bBuilt = false;
	BuiltSim = nullptr;
	BuiltGeneration = -1;
	LastSeconds = -1;
	ShownTop = -1;
	const int32 FullBuilds = Stats.FullBuilds;
	Stats = FBuildingStats();
	Stats.FullBuilds = FullBuilds;
}

int32 UBuildingSubsystem::GetChunkInstances(int32 ChunkX, int32 ChunkY) const
{
	if (!bBuilt || ChunkX < 0 || ChunkY < 0 || ChunkX >= ChunksX || ChunkY >= ChunksY)
	{
		return 0;
	}
	int32 Count = 0;
	for (int32 Level = 0; Level < colony::Map::kLevels; ++Level)
	{
		Count += Chunks[ChunkIndex(Level, ChunkX, ChunkY)].Instances;
	}
	return Count;
}

int32 UBuildingSubsystem::GetLevelInstances(int32 Level, bool bVisibleOnly) const
{
	int32 Count = 0;
	if (!bBuilt || Level < 0 || Level >= colony::Map::kLevels)
	{
		return 0;
	}
	for (int32 CY = 0; CY < ChunksY; ++CY)
	{
		for (int32 CX = 0; CX < ChunksX; ++CX)
		{
			for (const TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunks[ChunkIndex(Level, CX, CY)].Components)
			{
				if (Pair.Value && (!bVisibleOnly || Pair.Value->IsVisible()))
				{
					Count += Pair.Value->GetInstanceCount();
				}
			}
		}
	}
	return Count;
}

float UBuildingSubsystem::GetDoorShown(colony::Entity Door) const
{
	const FDoorSlot* Slot = DoorSlots.Find(static_cast<uint32>(Door));
	return Slot ? Slot->Shown : -1.0f;
}

float UBuildingSubsystem::GetDoorLeafOffsetCm(colony::Entity Door) const
{
	const FDoorSlot* Slot = DoorSlots.Find(static_cast<uint32>(Door));
	return Slot ? Slot->Shown * Slot->SlideCm : 0.0f;
}

float UBuildingSubsystem::GetDrawnHeightCm(colony::DefId Def) const
{
	const FDefLook* Look = LookOfDef(Def);
	return Look ? Look->HeightCm : 0.0f;
}

int32 UBuildingSubsystem::GetGhostInstances() const
{
	int32 Count = 0;
	for (const FChunk& Chunk : Chunks)
	{
		for (const TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunk.Components)
		{
			if ((Pair.Key & 3) == BldGhost && Pair.Value)
			{
				Count += Pair.Value->GetInstanceCount();
			}
		}
	}
	return Count;
}

UStaticMesh* UBuildingSubsystem::GetWholeMesh(colony::DefId Def, colony::DefId Stuff)
{
	const int32 LookId = bBuilt ? GetLook(Def, Stuff) : -1;
	return LookId >= 0 ? Looks[LookId].All : nullptr;
}

bool UBuildingSubsystem::GetPlacedTransform(colony::DefId Def, colony::CellPos Cell, colony::Rot4 Rot, FTransform& OutWorld) const
{
	const FDefLook* Look = bBuilt ? LookOfDef(Def) : nullptr;
	if (!Look || Look->bItem || !BuiltSim)
	{
		return false;
	}
	const colony::Map& Map = BuiltSim->map();
	const colony::CellRect Rect = colony::footprintRect(Cell, FMath::RoundToInt32(Look->FootprintCm.X / 100.0f), FMath::RoundToInt32(Look->FootprintCm.Y / 100.0f), Rot);
	// The lowest ground under the footprint, as the built building stands (an upper floor runs 3 m a level above it).
	const int32 Level = FMath::Clamp(Cell.level, 0, colony::Map::kLevels - 1);
	float Base = 1.0e9f;
	for (int32 Y = Rect.minY; Y <= Rect.minY + Rect.sizeY; ++Y)
	{
		for (int32 X = Rect.minX; X <= Rect.minX + Rect.sizeX; ++X)
		{
			Base = FMath::Min(Base, Map.groundHeight(colony::CellPos{FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1), Level}));
		}
	}
	const FVector Local((static_cast<float>(Rect.minX) + static_cast<float>(Rect.sizeX) * 0.5f) * 100.0f, (static_cast<float>(Rect.minY) + static_cast<float>(Rect.sizeY) * 0.5f) * 100.0f, Base * 100.0f);
	OutWorld = FTransform(FRotator(0.0, BldYawOf(Rot), 0.0), BuiltOrigin + Local);
	return true;
}

const UBuildingSubsystem::FDefLook* UBuildingSubsystem::LookOfDef(colony::DefId Def) const
{
	return Def.valid() && Def.value < static_cast<uint32>(DefLooks.Num()) && DefLooks[Def.value].bRead ? &DefLooks[Def.value] : nullptr;
}

void UBuildingSubsystem::ReadDefs(const colony::Sim& Sim)
{
	const colony::DefDatabase& Defs = Sim.defs();
	DefLooks.Reset();
	DefLooks.SetNum(static_cast<int32>(Defs.size()));
	for (const colony::DefId Id : Defs.idsOfKind("Things"))
	{
		const colony::Def& Def = Defs.get(Id);
		const std::string Category = Def.text("category");
		if (Category != "Building" && Category != "Item")
		{
			continue;
		}
		FDefLook& Look = DefLooks[static_cast<int32>(Id.value)];
		Look.bRead = true;
		Look.bItem = Category == "Item";
		Look.bDoor = Def.text("thingClass") == "Building_Door";
		if (!Look.bItem)
		{
			Look.FootprintCm = FVector2f(static_cast<float>(Def.numberAt("size", 0, 1.0)) * 100.0f, static_cast<float>(Def.numberAt("size", 1, 1.0)) * 100.0f);
		}
		VisualMesh::ReadParts(Def, "visual", Look.FootprintCm, Look.Parts);
		if (Look.Parts.IsEmpty())
		{
			// A def with no look yet still shows: a grey block, a metre high for a building and a handful for an item.
			VisualMesh::FPart Block;
			Block.Shape = VisualMesh::EShape::Box;
			Block.Size = Look.bItem ? FVector3f(25.0f, 25.0f, 25.0f) : FVector3f(Look.FootprintCm.X, Look.FootprintCm.Y, 100.0f);
			Block.Color = FLinearColor(0.35f, 0.35f, 0.35f);
			Look.Parts.Add(Block);
		}
		for (const VisualMesh::FPart& Part : Look.Parts)
		{
			if (Part.Slide > 0.0f)
			{
				Look.bHasSliding = true;
				Look.SlideCm = FMath::Max(Look.SlideCm, Part.Slide);
			}
		}
		Look.MaxPieces = FMath::Max(1, static_cast<int32>(Def.integer("visual.pieces", 1)));
		Look.LayerCm = static_cast<float>(Def.number("visual.layer", 0.1)) * 100.0f;
		Look.StackLimit = FMath::Max(1, static_cast<int32>(Def.integer("stackLimit", 1)));
		Look.HeightCm = VisualMesh::HeightOf(Look.Parts);

		// A mesh of the def's own (A1 makes them, tools/placeholder_art.py); without it the shapes above are drawn.
		const std::string MeshPath = Def.text("visual.mesh");
		if (!MeshPath.empty())
		{
			Look.ArtMesh = LoadObject<UStaticMesh>(nullptr, UTF8_TO_TCHAR(MeshPath.c_str()));
			if (Look.ArtMesh)
			{
				KeepMeshes.Add(Look.ArtMesh);
				if (!Look.bDoor)
				{
					Look.HeightCm = Look.ArtMesh->GetBounds().BoxExtent.Z * 2.0f;
				}
			}
			else
			{
				UE_LOG(LogSimBuildings, Warning, TEXT("%s: mesh %s not found, drawing its shapes instead (python tools/placeholder_art.py, then the PlaceholderArt commandlet)"), UTF8_TO_TCHAR(Def.defName().c_str()), UTF8_TO_TCHAR(MeshPath.c_str()));
			}
		}
	}
}

int32 UBuildingSubsystem::GetLook(colony::DefId Def, colony::DefId Stuff)
{
	const FDefLook* DefLook = LookOfDef(Def);
	if (!DefLook || !BuiltSim)
	{
		return -1;
	}
	const uint64 Key = (static_cast<uint64>(Def.value) << 32) | (Stuff.valid() ? static_cast<uint64>(Stuff.value) + 1 : 0);
	if (const int32* Found = LookIndex.Find(Key))
	{
		return *Found;
	}
	FLinearColor StuffColor(0.35f, 0.35f, 0.35f);
	if (Stuff.valid())
	{
		StuffColor = VisualMesh::ReadColor(BuiltSim->defs().get(Stuff), "visual.color", StuffColor);
	}
	FLook Look;
	Look.DefValue = static_cast<int32>(Def.value);
	const auto Make = [&](VisualMesh::EPartSet Set) -> UStaticMesh*
	{
		UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, DefLook->Parts, Set, &StuffColor, false);
		if (Mesh)
		{
			KeepMeshes.Add(Mesh);
			++Stats.Meshes;
		}
		return Mesh;
	};
	Look.Fixed = Make(DefLook->bHasSliding ? VisualMesh::EPartSet::Fixed : VisualMesh::EPartSet::All);
	if (DefLook->bHasSliding)
	{
		Look.Sliding = DefLook->bDoor && DefLook->ArtMesh ? DefLook->ArtMesh : Make(VisualMesh::EPartSet::Sliding);
		Look.All = Make(VisualMesh::EPartSet::All);
	}
	else if (DefLook->ArtMesh)
	{
		// Frames and ghosts keep the plain shapes: the ghost material replaces only the first material of a mesh.
		Look.All = Make(VisualMesh::EPartSet::All);
		Look.Fixed = DefLook->ArtMesh;
	}
	else
	{
		Look.All = Look.Fixed;
	}
	const int32 Index = Looks.Add(Look);
	LookIndex.Add(Key, Index);
	return Index;
}

UInstancedStaticMeshComponent* UBuildingSubsystem::GetComponent(FChunk& Chunk, int32 Key, UStaticMesh* Mesh, bool bGhost, bool bItem, bool bCastShadow)
{
	if (UInstancedStaticMeshComponent** Found = Chunk.Components.Find(Key))
	{
		return *Found;
	}
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(DrawActor);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(bCastShadow && !bGhost);
	Component->bAffectDistanceFieldLighting = false;
	if (bGhost && GhostMaterial)
	{
		Component->SetMaterial(0, GhostMaterial);
	}
	if (bItem)
	{
		Component->SetCullDistances(static_cast<int32>(BldItemCullCm * 0.8f), static_cast<int32>(BldItemCullCm));
	}
	Component->SetupAttachment(DrawActor->GetRootComponent());
	Component->RegisterComponent();
	Chunk.Components.Add(Key, Component);
	return Component;
}

void UBuildingSubsystem::RebuildAll()
{
	USimSubsystem* Sim = Bridge.Get();
	Teardown();
	if (!Sim || !Sim->GetSim() || !GetWorld())
	{
		return;
	}
	const colony::Sim& Core = *Sim->GetSim();
	BuiltSim = &Core;
	BuiltGeneration = Sim->GetSimGeneration();
	BuiltOrigin = Sim->GetWorldOrigin();
	ChunksX = (Core.map().width() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;
	ChunksY = (Core.map().height() + TerrainDraw::ChunkCells - 1) / TerrainDraw::ChunkCells;

	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	DrawActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), BuiltOrigin, FRotator::ZeroRotator, Params);
	USceneComponent* Root = NewObject<USceneComponent>(DrawActor, TEXT("BuildingsRoot"));
	DrawActor->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(BuiltOrigin);
#if WITH_EDITOR
	DrawActor->SetActorLabel(TEXT("Buildings"));
#endif

	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, BldSolidMaterialPath);
	if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, BldGhostMaterialPath))
	{
		UMaterialInstanceDynamic* Ghost = UMaterialInstanceDynamic::Create(Parent, this);
		Ghost->SetVectorParameterValue(TEXT("Color"), BldGhostColor);
		Ghost->SetScalarParameterValue(TEXT("Opacity"), BldGhostColor.A);
		GhostMaterial = Ghost;
	}
	else
	{
		// Made by the BuildingMaterials commandlet (docs/design/building-draw.md); without it plans are drawn solid.
		UE_LOG(LogSimBuildings, Warning, TEXT("%s not found: blueprints are drawn solid. Run the BuildingMaterials commandlet (assets/manifest.json)."), BldGhostMaterialPath);
		GhostMaterial = nullptr;
	}
	ReadDefs(Core);

	Chunks.SetNum(colony::Map::kLevels * ChunksX * ChunksY);
	ScanLists.SetNum(colony::Map::kLevels * ChunksX * ChunksY);
	bBuilt = true;
	++Stats.FullBuilds;
	Refresh();
}

void UBuildingSubsystem::Refresh()
{
	USimSubsystem* Sim = Bridge.Get();
	if (!bBuilt || !Sim || !Sim->GetSim() || Sim->GetSim() != BuiltSim)
	{
		return;
	}
	const colony::Snapshot& Snapshot = Sim->GetCurrentSnapshot();
	LastSeconds = Snapshot.gameSeconds;
	const colony::Map& Map = BuiltSim->map();

	for (TArray<int32>& List : ScanLists)
	{
		List.Reset();
	}
	ScanSignatures.Init(0, Chunks.Num());
	int32 Built = 0, Frames = 0, Blueprints = 0, Items = 0, Doors = 0;
	for (int32 I = 0; I < static_cast<int32>(Snapshot.things.size()); ++I)
	{
		const colony::ThingView& View = Snapshot.things[static_cast<size_t>(I)];
		if (View.category == colony::ThingCategory::Pawn)
		{
			continue;
		}
		const FDefLook* Look = LookOfDef(View.def);
		const int32 CX = BldChunkOf(View.x);
		const int32 CY = BldChunkOf(View.y);
		if (!Look || CX < 0 || CY < 0 || CX >= ChunksX || CY >= ChunksY || View.level < 0 || View.level >= colony::Map::kLevels)
		{
			continue;
		}
		uint64 H = BldCombine(static_cast<uint64>(static_cast<uint32>(View.id)), View.def.value);
		H = BldCombine(H, static_cast<uint64>(FMath::RoundToInt32(View.x * 4.0f)) * 100003ULL + static_cast<uint64>(FMath::RoundToInt32(View.y * 4.0f)));
		if (View.category == colony::ThingCategory::Item)
		{
			H = BldCombine(H, static_cast<uint64>(BldPieces(Look->MaxPieces, Look->StackLimit, View.stackCount)));
			++Items;
		}
		else
		{
			H = BldCombine(H, static_cast<uint64>(View.stage) * 16ULL + static_cast<uint64>(View.rotation));
			H = BldCombine(H, View.stuff.valid() ? View.stuff.value + 1ULL : 0ULL);
			switch (View.stage)
			{
				case colony::ConstructionStage::Built:
					++Built;
					break;
				case colony::ConstructionStage::Frame:
					++Frames;
					H = BldCombine(H, static_cast<uint64>(BldFrameScale(View) * 1000.0f));
					break;
				case colony::ConstructionStage::Blueprint:
					++Blueprints;
					break;
			}
			if (Look->bDoor && View.stage == colony::ConstructionStage::Built)
			{
				++Doors;
				H = BldCombine(H, BldDoorAlongY(Map, View) ? 1ULL : 2ULL);
			}
		}
		const int32 Chunk = ChunkIndex(View.level, CX, CY);
		ScanSignatures[Chunk] += H;
		ScanLists[Chunk].Add(I);
	}
	Stats.Built = Built;
	Stats.Frames = Frames;
	Stats.Blueprints = Blueprints;
	Stats.Items = Items;
	Stats.Doors = Doors;

	bool bRebuilt = false;
	for (int32 Level = 0; Level < colony::Map::kLevels; ++Level)
	{
		for (int32 CY = 0; CY < ChunksY; ++CY)
		{
			for (int32 CX = 0; CX < ChunksX; ++CX)
			{
				const int32 Chunk = ChunkIndex(Level, CX, CY);
				if (ScanSignatures[Chunk] != Chunks[Chunk].Signature)
				{
					RebuildChunk(Level, CX, CY, ScanLists[Chunk], Snapshot);
					Chunks[Chunk].Signature = ScanSignatures[Chunk];
					bRebuilt = true;
				}
			}
		}
	}
	// A rebuilt chunk may have new components, which start shown.
	ApplyCut(bRebuilt);
}

void UBuildingSubsystem::ApplyCut(bool bForce)
{
	const int32 Top = ULevelsSubsystem::TopShownLevel(GetWorld());
	if (!bForce && Top == ShownTop)
	{
		return;
	}
	ShownTop = Top;
	for (int32 Index = 0; Index < Chunks.Num(); ++Index)
	{
		const bool bShow = LevelOfChunk(Index) <= Top;
		for (TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunks[Index].Components)
		{
			if (Pair.Value && Pair.Value->IsVisible() != bShow)
			{
				Pair.Value->SetVisibility(bShow);
			}
		}
	}
}

void UBuildingSubsystem::RebuildChunk(int32 Level, int32 ChunkX, int32 ChunkY, const TArray<int32>& ThingIndexes, const colony::Snapshot& Snapshot)
{
	const double Start = FPlatformTime::Seconds();
	FChunk& Chunk = Chunks[ChunkIndex(Level, ChunkX, ChunkY)];
	const colony::Map& Map = BuiltSim->map();

	TMap<int32, FBldBatch> Batches;
	struct FPendingDoor
	{
		uint32 Id;
		int32 Key;
		int32 Index;
		FTransform Base;
		FVector Direction;
		float SlideCm;
	};
	TArray<FPendingDoor> Pending;

	const auto Add = [&Batches](int32 Look, int32 Variant, UStaticMesh* Mesh, bool bGhost, bool bItem, bool bShadow, const FTransform& Transform) -> int32
	{
		FBldBatch& Batch = Batches.FindOrAdd(Look * 4 + Variant);
		Batch.Mesh = Mesh;
		Batch.bGhost = bGhost;
		Batch.bItem = bItem;
		Batch.bShadow = bShadow;
		return Batch.Transforms.Add(Transform);
	};

	for (const int32 ThingIndex : ThingIndexes)
	{
		const colony::ThingView& View = Snapshot.things[static_cast<size_t>(ThingIndex)];
		const FDefLook* DefLook = LookOfDef(View.def);
		if (!DefLook)
		{
			continue;
		}
		if (View.category == colony::ThingCategory::Item)
		{
			const int32 ItemLook = GetLook(View.def, colony::DefId());
			if (ItemLook < 0 || !Looks[ItemLook].Fixed)
			{
				continue;
			}
			UStaticMesh* Mesh = Looks[ItemLook].Fixed;
			const int32 Pieces = BldPieces(DefLook->MaxPieces, DefLook->StackLimit, View.stackCount);
			const uint64 Seed = BldMix(static_cast<uint64>(static_cast<uint32>(View.id)) + 0x51ED27ULL);
			for (int32 P = 0; P < Pieces; ++P)
			{
				const uint64 H = BldMix(Seed + static_cast<uint64>(P) * 7919ULL);
				const float J1 = (static_cast<float>(H & 0xFFFF) / 65535.0f - 0.5f) * 10.0f;
				const float J2 = (static_cast<float>((H >> 16) & 0xFFFF) / 65535.0f - 0.5f) * 10.0f;
				const float Yaw = static_cast<float>((H >> 32) & 0xFFFF) / 65535.0f * 360.0f;
				// Pieces sit in a 2 x 2 square, then pile on top of it.
				const int32 Slot = P % 4;
				const float OffsetX = Pieces > 1 ? ((Slot & 1) ? 17.0f : -17.0f) : 0.0f;
				const float OffsetY = Pieces > 1 ? ((Slot & 2) ? 15.0f : -15.0f) : 0.0f;
				const float Lift = static_cast<float>(P / 4) * DefLook->LayerCm;
				const FVector Location(View.x * 100.0f + OffsetX + J1, View.y * 100.0f + OffsetY + J2, View.z * 100.0f + Lift);
				Add(ItemLook, BldFixed, Mesh, false, true, false, FTransform(FRotator(0.0, Yaw, 0.0), Location));
			}
			continue;
		}

		// A building, a blueprint or a frame: standing on the lowest ground under its footprint, so a slope never leaves a gap
		// (upstairs, on the floor 3 m a level above that ground).
		const int32 OriginX = FMath::RoundToInt32(View.x - static_cast<float>(View.sizeX) * 0.5f);
		const int32 OriginY = FMath::RoundToInt32(View.y - static_cast<float>(View.sizeY) * 0.5f);
		float Base = View.z;
		for (int32 Y = OriginY; Y <= OriginY + View.sizeY; ++Y)
		{
			for (int32 X = OriginX; X <= OriginX + View.sizeX; ++X)
			{
				Base = FMath::Min(Base, Map.groundHeight(colony::CellPos{FMath::Clamp(X, 0, Map.width() - 1), FMath::Clamp(Y, 0, Map.height() - 1), Level}));
			}
		}
		const bool bDoorBuilt = DefLook->bDoor && View.stage == colony::ConstructionStage::Built;
		const float Yaw = bDoorBuilt ? (BldDoorAlongY(Map, View) ? 90.0f : 0.0f) : BldYawOf(View.rotation);
		const FVector Location(View.x * 100.0f, View.y * 100.0f, Base * 100.0f);
		const int32 LookId = GetLook(View.def, View.stuff);
		if (LookId < 0)
		{
			continue;
		}
		const FLook Look = Looks[LookId];
		const FTransform Placed(FRotator(0.0, Yaw, 0.0), Location);
		switch (View.stage)
		{
			case colony::ConstructionStage::Built:
				if (Look.Fixed)
				{
					Add(LookId, BldFixed, Look.Fixed, false, false, true, Placed);
				}
				if (Look.Sliding)
				{
					const int32 Index = Add(LookId, BldSliding, Look.Sliding, false, false, true, Placed);
					Pending.Add({static_cast<uint32>(View.id), LookId * 4 + BldSliding, Index, Placed, FRotator(0.0, Yaw, 0.0).RotateVector(FVector::XAxisVector), DefLook->SlideCm});
				}
				break;
			case colony::ConstructionStage::Frame:
			{
				// The part-built building inside the ghost of the whole.
				FTransform Part = Placed;
				Part.SetScale3D(FVector(1.0, 1.0, BldFrameScale(View)));
				if (Look.All)
				{
					Add(LookId, BldAll, Look.All, false, false, true, Part);
					Add(LookId, BldGhost, Look.All, true, false, false, Placed);
				}
				break;
			}
			case colony::ConstructionStage::Blueprint:
				if (Look.All)
				{
					Add(LookId, BldGhost, Look.All, true, false, false, Placed);
				}
				break;
		}
	}

	// Put the new instances in the chunk's components, making components for kinds the chunk has not had yet.
	for (const colony::Entity Door : Chunk.Doors)
	{
		DoorSlots.Remove(static_cast<uint32>(Door));
	}
	Chunk.Doors.Reset();
	Stats.Instances -= Chunk.Instances;
	int32 Total = 0;
	for (TPair<int32, UInstancedStaticMeshComponent*>& Pair : Chunk.Components)
	{
		if (Pair.Value && Pair.Value->GetInstanceCount() > 0)
		{
			--Stats.Components;
			Pair.Value->ClearInstances();
		}
	}
	for (TPair<int32, FBldBatch>& Pair : Batches)
	{
		FBldBatch& Batch = Pair.Value;
		UInstancedStaticMeshComponent* Component = GetComponent(Chunk, Pair.Key, Batch.Mesh, Batch.bGhost, Batch.bItem, Batch.bShadow);
		Component->AddInstances(Batch.Transforms, false, false, false);
		++Stats.Components;
		Total += Batch.Transforms.Num();
	}
	for (const FPendingDoor& Door : Pending)
	{
		FDoorSlot Slot;
		Slot.Component = Chunk.Components.FindRef(Door.Key);
		Slot.Index = Door.Index;
		Slot.Base = Door.Base;
		Slot.Direction = Door.Direction;
		Slot.SlideCm = Door.SlideCm;
		Slot.Shown = 0.0f;
		DoorSlots.Add(Door.Id, Slot);
		Chunk.Doors.Add(static_cast<colony::Entity>(Door.Id));
	}
	Chunk.Instances = Total;
	Stats.Instances += Total;
	++Stats.ChunkRebuilds;
	Stats.SlowestChunkMs = FMath::Max(Stats.SlowestChunkMs, (FPlatformTime::Seconds() - Start) * 1000.0);
}

void UBuildingSubsystem::AnimateDoors()
{
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || DoorSlots.IsEmpty())
	{
		return;
	}
	const colony::Snapshot& Current = Sim->GetCurrentSnapshot();
	const colony::Snapshot& Previous = Sim->GetPreviousSnapshot();
	const float Alpha = Sim->GetBlendAlpha();
	TSet<UInstancedStaticMeshComponent*> Touched;
	for (TPair<uint32, FDoorSlot>& Pair : DoorSlots)
	{
		FDoorSlot& Slot = Pair.Value;
		const colony::Entity Id = static_cast<colony::Entity>(Pair.Key);
		const colony::ThingView* Now = Current.find(Id);
		if (!Now || !Slot.Component)
		{
			continue;
		}
		const colony::ThingView* Before = Previous.find(Id);
		const float Open = FMath::Clamp(Before ? FMath::Lerp(Before->doorOpen, Now->doorOpen, Alpha) : Now->doorOpen, 0.0f, 1.0f);
		if (FMath::Abs(Open - Slot.Shown) < 0.002f)
		{
			continue;
		}
		Slot.Shown = Open;
		FTransform Moved = Slot.Base;
		Moved.AddToTranslation(Slot.Direction * (Open * Slot.SlideCm));
		Slot.Component->UpdateInstanceTransform(Slot.Index, Moved, false, false, true);
		Touched.Add(Slot.Component);
	}
	for (UInstancedStaticMeshComponent* Component : Touched)
	{
		Component->MarkRenderStateDirty();
	}
}

void UBuildingSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Sim = Bridge.Get();
	if (!Sim || !Sim->GetSim() || GBuildingsEnable.GetValueOnGameThread() == 0)
	{
		if (bBuilt || DrawActor)
		{
			Teardown();
		}
		return;
	}
	if (!bBuilt || BuiltGeneration != Sim->GetSimGeneration() || BuiltSim != Sim->GetSim())
	{
		RebuildAll();
	}
	else if (Sim->GetCurrentSnapshot().gameSeconds != LastSeconds)
	{
		Refresh();
	}
	if (Sim->GetWorldOrigin() != BuiltOrigin && DrawActor)
	{
		BuiltOrigin = Sim->GetWorldOrigin();
		DrawActor->SetActorLocation(BuiltOrigin);
	}
	ApplyCut(false);
	AnimateDoors();
}

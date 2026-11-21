#include "WreckSubsystem.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"

#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include "BlastFxSubsystem.h"
#include "SimSubsystem.h"
#include "TerrainDraw.h"
#include "VisualMesh.h"
#include "WetnessShared.h"

namespace
{
	TAutoConsoleVariable<int32> GWreckEnabled(
		TEXT("Wreck.Enabled"), 1, TEXT("1: a building the sim wrecks falls apart into pieces and dust; 0: it just goes."), ECVF_Default);
	TAutoConsoleVariable<int32> GWreckMaxPieces(
		TEXT("Wreck.MaxPieces"), 1500, TEXT("Most pieces of wrecked buildings lying about at once; the oldest wreck is cleared first."), ECVF_Default);

	const TCHAR* const WreckSurfaceFolder = TEXT("/Game/Rim3D/Generated/Surfaces");
	// Seconds the pieces tumble under physics before they are frozen where they lie as rubble.
	constexpr float WreckSettleSeconds = 6.0f;
	// The invisible floor the pieces fall on (the ground is drawn from the sim and has no collision of its own), cm thick.
	constexpr float WreckFloorThicknessCm = 20.0f;
}

bool UWreckSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UWreckSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UWreckSubsystem, STATGROUP_Tickables);
}

void UWreckSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);
	if (USimSubsystem* Sim = InCollection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Sim;
		EventHandle = Sim->OnSimEvent.AddUObject(this, &UWreckSubsystem::OnSimEvent);
	}
	InCollection.InitializeDependency<UBlastFxSubsystem>();
}

void UWreckSubsystem::Deinitialize()
{
	if (USimSubsystem* Sim = Bridge.Get())
	{
		Sim->OnSimEvent.Remove(EventHandle);
	}
	Teardown();
	Super::Deinitialize();
}

void UWreckSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("WreckRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("Wrecks"));
#endif
	SolidMaterial = WetnessShared::LoadSolidMaterial();
}

void UWreckSubsystem::Teardown()
{
	for (FWreck& Ruin : Wrecks)
	{
		ClearWreck(Ruin);
	}
	Wrecks.Reset();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
}

void UWreckSubsystem::ClearWreck(FWreck& Ruin)
{
	for (const TWeakObjectPtr<UStaticMeshComponent>& Piece : Ruin.Pieces)
	{
		if (Piece.IsValid())
		{
			Piece->DestroyComponent();
			++PiecesCleared;
		}
	}
	Ruin.Pieces.Reset();
	if (Ruin.Floor.IsValid())
	{
		Ruin.Floor->DestroyComponent();
	}
}

int32 UWreckSubsystem::GetPiecesAlive() const
{
	int32 Count = 0;
	for (const FWreck& Ruin : Wrecks)
	{
		for (const TWeakObjectPtr<UStaticMeshComponent>& Piece : Ruin.Pieces)
		{
			Count += Piece.IsValid() ? 1 : 0;
		}
	}
	return Count;
}

UStaticMesh* UWreckSubsystem::PieceMesh(colony::DefId Stuff)
{
	const int32 Key = Stuff.valid() ? static_cast<int32>(Stuff.value) : -1;
	if (const TObjectPtr<UStaticMesh>* Found = MeshByStuff.Find(Key))
	{
		return *Found;
	}
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	// The stuff's own surface (the SurfaceMaterials commandlet's MI_Surface_<surface>, as the building was drawn with), so
	// a granite wall breaks into granite and a wooden one into wood; its plain colour where there is none; steel grey
	// for a building made of no stuff (a stove, a turret).
	UMaterialInterface* Surface = nullptr;
	FLinearColor Colour(0.32f, 0.33f, 0.34f);
	if (Sim && Stuff.valid())
	{
		const colony::Def& Def = Sim->defs().get(Stuff);
		Colour = VisualMesh::ReadColor(Def, "visual.color", Colour);
		const std::string Name = Def.text("visual.surface");
		if (!Name.empty())
		{
			const FString Asset = FString::Printf(TEXT("MI_Surface_%s"), UTF8_TO_TCHAR(Name.c_str()));
			Surface = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s/%s.%s"), WreckSurfaceFolder, *Asset, *Asset), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
	}
	VisualMesh::FPart Part;
	Part.Shape = VisualMesh::EShape::Box;
	Part.Size = FVector3f(10.0f, 10.0f, 10.0f);
	Part.Color = Surface ? FLinearColor(0.85f, 0.85f, 0.85f) : FLinearColor::White;
	Part.bStuff = true;
	TArray<VisualMesh::FPart> Parts;
	Parts.Add(Part);
	UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, &Colour, false, Surface);
	if (Mesh)
	{
		// A mesh made at run time has no collision: give each piece a box to be a rigid body with (a 10 cm cube on z = 0).
		if (!Mesh->GetBodySetup())
		{
			Mesh->CreateBodySetup();
		}
		if (UBodySetup* Setup = Mesh->GetBodySetup())
		{
			Setup->RemoveSimpleCollision();
			FKBoxElem Cube(10.0f, 10.0f, 10.0f);
			Cube.Center = FVector(0.0, 0.0, 5.0);
			Setup->AggGeom.BoxElems.Add(Cube);
			Setup->CollisionTraceFlag = CTF_UseSimpleAsComplex;
			Setup->InvalidatePhysicsData();
			Setup->CreatePhysicsMeshes();
		}
		KeepMeshes.Add(Mesh);
	}
	MeshByStuff.Add(Key, Mesh);
	return Mesh;
}

void UWreckSubsystem::Wreck(colony::DefId Def, colony::DefId Stuff, float MapX, float MapY, float GroundM, int32 SizeX, int32 SizeY, int32 Level)
{
	USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim || GWreckEnabled.GetValueOnGameThread() == 0)
	{
		return;
	}
	EnsureScene();
	if (!Holder || !SolidMaterial)
	{
		return;
	}
	UStaticMesh* Mesh = PieceMesh(Stuff);
	if (!Mesh)
	{
		return;
	}
	const colony::Map& Map = Sim->map();
	SizeX = FMath::Max(1, SizeX);
	SizeY = FMath::Max(1, SizeY);
	// How tall it stood: a wall (fills its cell) up to the roof, other things by how much of the cell they fill.
	float Fill = 0.5f;
	if (Def.valid())
	{
		Fill = static_cast<float>(Sim->defs().get(Def).number("fillPercent", 0.5));
	}
	const float HeightCm = Fill >= 0.99f ? TerrainDraw::RoofHeightM * 100.0f : FMath::Clamp(Fill * 150.0f, 50.0f, 150.0f);

	const FVector Origin = Source->GetWorldOrigin();
	const FVector Centre = Origin + FVector(MapX * 100.0, MapY * 100.0, GroundM * 100.0);

	FWreck Ruin;
	Ruin.Level = Level;
	const int32 MinX = FMath::FloorToInt32(MapX - 0.5f * static_cast<float>(SizeX) + 0.01f);
	const int32 MinY = FMath::FloorToInt32(MapY - 0.5f * static_cast<float>(SizeY) + 0.01f);
	for (int32 DY = 0; DY < SizeY; ++DY)
	{
		for (int32 DX = 0; DX < SizeX; ++DX)
		{
			const colony::CellPos Cell{MinX + DX, MinY + DY, 0};
			if (Map.inBounds(Cell))
			{
				Ruin.Cells.Add(Map.index(Cell));
			}
		}
	}

	// The floor the pieces land on, a little wider than the footprint so pieces thrown out stay up.
	UStaticMeshComponent* Floor = NewObject<UStaticMeshComponent>(Holder);
	Floor->SetStaticMesh(Mesh);
	Floor->SetMobility(EComponentMobility::Movable);
	Floor->SetCollisionProfileName(TEXT("BlockAll"));
	Floor->SetCollisionObjectType(ECC_WorldStatic);
	Floor->SetCanEverAffectNavigation(false);
	Floor->SetCastShadow(false);
	Floor->SetVisibility(false);
	Floor->SetHiddenInGame(true);
	const float FloorSpanCm = (static_cast<float>(FMath::Max(SizeX, SizeY)) + 4.0f) * 100.0f;
	Floor->SetWorldScale3D(FVector(FloorSpanCm / 10.0f, FloorSpanCm / 10.0f, WreckFloorThicknessCm / 10.0f));
	Floor->RegisterComponent();
	Floor->SetWorldLocation(Centre - FVector(0.0, 0.0, WreckFloorThicknessCm));
	Ruin.Floor = Floor;

	// Pieces where the building stood, through its whole height, falling mostly straight down and a little outward.
	const int32 Cells = SizeX * SizeY;
	const float PerCell = FMath::Lerp(6.0f, 18.0f, FMath::Clamp(HeightCm / 300.0f, 0.0f, 1.0f));
	const int32 Want = FMath::Clamp(FMath::RoundToInt(PerCell * static_cast<float>(Cells)), 4, 120);
	UBlastFxSubsystem* Fx = GetWorld()->GetSubsystem<UBlastFxSubsystem>();
	const bool bDust = Fx && Fx->IsReady();
	for (int32 I = 0; I < Want; ++I)
	{
		UStaticMeshComponent* Piece = NewObject<UStaticMeshComponent>(Holder);
		Piece->SetStaticMesh(Mesh);
		Piece->SetMobility(EComponentMobility::Movable);
		Piece->SetCollisionProfileName(TEXT("PhysicsActor"));
		// Pieces land on the floor and on each other, not on pawns.
		Piece->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		Piece->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
		Piece->SetCanEverAffectNavigation(false);
		Piece->SetCastShadow(true);
		Piece->bReceivesDecals = true;
		// Broken blocks and slabs: mostly hand-sized, some as big as a brick course, a few big chunks.
		const float Size = FMath::Lerp(0.8f, 3.6f, FMath::Pow(Random.FRand(), 2.2f));
		Piece->SetWorldScale3D(FVector(Size * Random.FRandRange(0.6f, 1.6f), Size * Random.FRandRange(0.5f, 1.2f), Size * Random.FRandRange(0.3f, 0.9f)));
		const FVector Start = Centre + FVector(Random.FRandRange(-50.0f, 50.0f) * static_cast<float>(SizeX), Random.FRandRange(-50.0f, 50.0f) * static_cast<float>(SizeY),
			Random.FRandRange(10.0f, FMath::Max(20.0f, HeightCm - 20.0f)));
		Piece->SetWorldLocationAndRotation(Start, FRotator(Random.FRand() * 360.0f, Random.FRand() * 360.0f, Random.FRand() * 360.0f));
		Piece->RegisterComponent();
		Piece->SetSimulatePhysics(true);
		Piece->SetEnableGravity(true);
		Piece->SetLinearDamping(0.3f);
		Piece->SetAngularDamping(0.6f);
		Piece->SetMassOverrideInKg(NAME_None, 2.0f * Size * Size * Size, true);
		FVector Out = (Start - Centre).GetSafeNormal2D();
		if (Out.IsNearlyZero())
		{
			Out = Random.VRand().GetSafeNormal2D();
		}
		Piece->SetPhysicsLinearVelocity(Out * Random.FRandRange(30.0f, 180.0f) + FVector(0.0, 0.0, Random.FRandRange(-40.0f, 60.0f)));
		Piece->SetPhysicsAngularVelocityInDegrees(Random.VRand() * Random.FRandRange(30.0f, 240.0f));
		Ruin.Pieces.Add(Piece);
	}
	// Dust bursts out of the falling building and rolls over the rubble (the blast art's smoke sprites).
	if (bDust)
	{
		const int32 Puffs = FMath::Clamp(3 + Cells * 3, 4, 24);
		for (int32 I = 0; I < Puffs; ++I)
		{
			const FVector At = Centre + FVector(Random.FRandRange(-60.0f, 60.0f) * static_cast<float>(SizeX), Random.FRandRange(-60.0f, 60.0f) * static_cast<float>(SizeY),
				Random.FRandRange(20.0f, HeightCm * 0.8f));
			Fx->AddDebrisTrail(At, Random.FRandRange(70.0f, 140.0f) * FMath::Clamp(HeightCm / 200.0f, 0.6f, 1.4f));
		}
	}
	Wrecks.Add(MoveTemp(Ruin));
	++WrecksMade;

	// Keep within the budget: the oldest wrecks go first.
	const int32 Budget = FMath::Max(100, GWreckMaxPieces.GetValueOnGameThread());
	while (Wrecks.Num() > 1 && GetPiecesAlive() > Budget)
	{
		ClearWreck(Wrecks[0]);
		Wrecks.RemoveAt(0);
	}
}

void UWreckSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::BuildingWrecked)
	{
		return;
	}
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim)
	{
		return;
	}
	const colony::DefId Stuff = Event.message.empty() ? colony::DefId{} : Sim->defs().idOf("Things", Event.message);
	Wreck(Event.def, Stuff, Event.x, Event.y, Event.z, Event.count, Event.part, Event.cell.level);
}

void UWreckSubsystem::ClearCleanedRubble()
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim)
	{
		return;
	}
	const colony::Snapshot& Snapshot = Source->GetCurrentSnapshot();
	if (Snapshot.filthChanges == SeenFilthChanges)
	{
		return;
	}
	SeenFilthChanges = Snapshot.filthChanges;
	const colony::DefId Rubble = Sim->defs().idOf("Filth", "Rubble");
	TSet<int32> RubbleCells;
	for (const colony::FilthView& Filth : Snapshot.filth)
	{
		if (Filth.def == Rubble && Sim->map().inBounds(Filth.cell))
		{
			RubbleCells.Add(Sim->map().index(Filth.cell));
		}
	}
	// A settled wreck whose rubble is all cleaned away goes with it. (Upper floors keep no filth: their pieces stay.)
	for (int32 Index = Wrecks.Num() - 1; Index >= 0; --Index)
	{
		FWreck& Ruin = Wrecks[Index];
		if (!Ruin.bSettled || Ruin.Level != 0 || Ruin.Cells.IsEmpty())
		{
			continue;
		}
		bool bAny = false;
		for (const int32 Cell : Ruin.Cells)
		{
			bAny = bAny || RubbleCells.Contains(Cell);
		}
		if (!bAny)
		{
			ClearWreck(Ruin);
			Wrecks.RemoveAt(Index);
		}
	}
}

void UWreckSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	if (BuiltGeneration != Source->GetSimGeneration())
	{
		// A new game or a load: the old wrecks belong to another map.
		BuiltGeneration = Source->GetSimGeneration();
		Teardown();
		SeenFilthChanges = ~0ULL;
	}
	// Physics runs in real time; once the pieces have had time to fall they are frozen where they lie, and the floor goes.
	for (FWreck& Ruin : Wrecks)
	{
		Ruin.Age += DeltaTime;
		if (Ruin.bSettled || Ruin.Age < WreckSettleSeconds)
		{
			continue;
		}
		Ruin.bSettled = true;
		for (const TWeakObjectPtr<UStaticMeshComponent>& Piece : Ruin.Pieces)
		{
			if (Piece.IsValid())
			{
				Piece->SetSimulatePhysics(false);
				Piece->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				++PiecesSettled;
			}
		}
		if (Ruin.Floor.IsValid())
		{
			Ruin.Floor->DestroyComponent();
		}
	}
	ClearCleanedRubble();
}

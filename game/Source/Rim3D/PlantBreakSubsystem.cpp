#include "PlantBreakSubsystem.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

#include "ColonySim/Defs.h"
#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include "SimSubsystem.h"
#include "TerrainSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> GPlantBreakEnabled(
		TEXT("PlantBreak.Enabled"), 1, TEXT("1: cut grass and bushes break up into clippings that fade; 0: they just go."), ECVF_Default);
	TAutoConsoleVariable<float> GPlantBreakSeconds(
		TEXT("PlantBreak.Seconds"), 10.0f, TEXT("Seconds from the cut until the last clipping is gone."), ECVF_Default);
	TAutoConsoleVariable<int32> GPlantBreakMax(
		TEXT("PlantBreak.Max"), 120, TEXT("Most clippings at once; the oldest go first."), ECVF_Default);

	constexpr float PlantBreakGravityCm = 980.0f;
	// Clippings are a little light and leafy: the air slows them (share of the speed a second).
	constexpr float PlantBreakAirDrag = 1.2f;
	// The share of the life they lie whole before they start to shrink away.
	constexpr float PlantBreakShrinkFrom = 0.4f;
	// How far they sink while they shrink (cm).
	constexpr float PlantBreakSinkCm = 6.0f;
}

bool UPlantBreakSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UPlantBreakSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UPlantBreakSubsystem, STATGROUP_Tickables);
}

void UPlantBreakSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);
	if (USimSubsystem* Sim = InCollection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Sim;
		EventHandle = Sim->OnSimEvent.AddUObject(this, &UPlantBreakSubsystem::OnSimEvent);
	}
	InCollection.InitializeDependency<UTerrainSubsystem>();
}

void UPlantBreakSubsystem::Deinitialize()
{
	if (USimSubsystem* Sim = Bridge.Get())
	{
		Sim->OnSimEvent.Remove(EventHandle);
	}
	Teardown();
	Super::Deinitialize();
}

void UPlantBreakSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("PlantBreakRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("PlantClippings"));
#endif
}

void UPlantBreakSubsystem::Teardown()
{
	for (FPiece& Piece : Pieces)
	{
		if (Piece.Mesh.IsValid())
		{
			Piece.Mesh->DestroyComponent();
		}
	}
	Pieces.Reset();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
}

void UPlantBreakSubsystem::RemovePiece(int32 Index)
{
	if (Pieces[Index].Mesh.IsValid())
	{
		Pieces[Index].Mesh->DestroyComponent();
	}
	Pieces.RemoveAt(Index);
}

float UPlantBreakSubsystem::GroundZ(const FVector& At) const
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	if (!Sim)
	{
		return static_cast<float>(At.Z);
	}
	const FVector Origin = Source->GetWorldOrigin();
	const FVector M = (At - Origin) / 100.0;
	const colony::Map& Map = Sim->map();
	const float X = FMath::Clamp(static_cast<float>(M.X), 0.0f, static_cast<float>(Map.width()) - 0.01f);
	const float Y = FMath::Clamp(static_cast<float>(M.Y), 0.0f, static_cast<float>(Map.height()) - 0.01f);
	return static_cast<float>(Origin.Z) + Map.heightAt(X, Y) * 100.0f;
}

void UPlantBreakSubsystem::PlayEventForTest(const colony::Event& Event)
{
	OnSimEvent(Event);
}

void UPlantBreakSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::PlantRemoved || GPlantBreakEnabled.GetValueOnGameThread() == 0 ||
		(Event.message != "cut" && Event.message != "harvested" && Event.message != "broken"))
	{
		return;
	}
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	const UTerrainSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainSubsystem>() : nullptr;
	if (!Sim || !Terrain || !Event.def.valid() || Sim->defs().get(Event.def).flag("plant.isTree"))
	{
		return;  // trees fall (UTreeFallSubsystem)
	}
	// Its growth comes with the event (a cut plant's); a broken one has none, so it is drawn grown.
	const uint32 Growth = Event.count > 0 ? static_cast<uint32>(Event.count) : 1000000u;
	FTerrainPlantLook Look;
	if (!Terrain->GetPlantLook(Event.cell, Event.def, Growth, Look) || !Look.Mesh)
	{
		return;
	}
	EnsureScene();
	if (!Holder)
	{
		return;
	}
	++BreaksStarted;

	const FBox Bounds = Look.Mesh->GetBounds().GetBox();
	const FVector PlantScale = Look.Transform.GetScale3D();
	const float HeightCm = FMath::Max(10.0f, static_cast<float>(Bounds.Max.Z * PlantScale.Z));
	const float WidthCm = FMath::Max(10.0f, static_cast<float>(FMath::Max(Bounds.GetExtent().X * PlantScale.X, Bounds.GetExtent().Y * PlantScale.Y)));
	// A tuft of grass makes four clippings, a bush six.
	const int32 Count = HeightCm < 60.0f ? 4 : 6;
	const FVector Base = Look.Transform.GetLocation();
	const FQuat Standing = Look.Transform.GetRotation();
	for (int32 I = 0; I < Count; ++I)
	{
		const float Angle = (static_cast<float>(I) + Random.FRandRange(0.0f, 0.8f)) / static_cast<float>(Count) * 2.0f * PI;
		const FVector Out(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		FPiece Piece;
		Piece.Location = Base + Out * Random.FRandRange(0.1f, 0.35f) * WidthCm + FVector(0.0, 0.0, Random.FRandRange(0.05f, 0.35f) * HeightCm);
		Piece.Velocity = Out * Random.FRandRange(50.0f, 150.0f) + FVector(0.0, 0.0, Random.FRandRange(120.0f, 260.0f));
		Piece.Rotation = FRotator(Random.FRandRange(-35.0f, 35.0f), Random.FRandRange(0.0f, 360.0f), Random.FRandRange(-35.0f, 35.0f)).Quaternion() * Standing;
		Piece.SpinAxis = FVector(Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-1.0f, 1.0f), Random.FRandRange(-0.4f, 0.4f)).GetSafeNormal();
		if (Piece.SpinAxis.IsNearlyZero())
		{
			Piece.SpinAxis = FVector::XAxisVector;
		}
		Piece.SpinRate = Random.FRandRange(2.0f, 6.0f);
		Piece.Scale = PlantScale * FVector(Random.FRandRange(0.4f, 0.6f), Random.FRandRange(0.4f, 0.6f), Random.FRandRange(0.3f, 0.5f));

		UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(Holder);
		Mesh->SetStaticMesh(Look.Mesh);
		for (int32 Slot = 0; Slot < Look.Materials.Num(); ++Slot)
		{
			if (Look.Materials[Slot])
			{
				Mesh->SetMaterial(Slot, Look.Materials[Slot]);
			}
		}
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCanEverAffectNavigation(false);
		Mesh->SetCastShadow(false);
		Mesh->bAffectDistanceFieldLighting = false;
		if (Look.WindOffCm > 0.0f)
		{
			Mesh->SetWorldPositionOffsetDisableDistance(static_cast<int32>(Look.WindOffCm));
		}
		Mesh->SetupAttachment(Holder->GetRootComponent());
		Mesh->RegisterComponent();
		Mesh->SetWorldTransform(FTransform(Piece.Rotation, Piece.Location, Piece.Scale));
		Piece.Mesh = Mesh;
		Pieces.Add(Piece);
	}

	// Within the budget: the oldest clippings go.
	const int32 Budget = FMath::Max(Count, GPlantBreakMax.GetValueOnGameThread());
	while (Pieces.Num() > Budget)
	{
		RemovePiece(0);
	}
}

void UPlantBreakSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	if (BuiltGeneration != Source->GetSimGeneration())
	{
		// A new game or a load: the clippings belong to another map.
		BuiltGeneration = Source->GetSimGeneration();
		Teardown();
	}
	if (Pieces.IsEmpty() || Source->GetSpeed() <= 0)
	{
		return;  // paused: everything waits where it is
	}
	const float Dt = FMath::Min(DeltaTime, 0.1f);
	const float Life = FMath::Max(1.0f, GPlantBreakSeconds.GetValueOnGameThread());
	for (int32 Index = Pieces.Num() - 1; Index >= 0; --Index)
	{
		FPiece& Piece = Pieces[Index];
		if (!Piece.Mesh.IsValid())
		{
			Pieces.RemoveAt(Index);
			continue;
		}
		Piece.Age += Dt;
		if (Piece.Age >= Life)
		{
			RemovePiece(Index);
			continue;
		}
		if (!Piece.bLanded)
		{
			Piece.Velocity.Z -= PlantBreakGravityCm * Dt;
			Piece.Velocity *= FMath::Max(0.0f, 1.0f - PlantBreakAirDrag * Dt);
			Piece.Location += Piece.Velocity * Dt;
			Piece.Rotation = FQuat(Piece.SpinAxis, Piece.SpinRate * Dt) * Piece.Rotation;
			const float Ground = GroundZ(Piece.Location);
			if (Piece.Location.Z <= Ground)
			{
				Piece.Location.Z = Ground;
				if (Piece.Bounces == 0 && Piece.Velocity.Z < -80.0f)
				{
					// One small bounce, then it lies where it lands.
					++Piece.Bounces;
					Piece.Velocity = FVector(Piece.Velocity.X * 0.35, Piece.Velocity.Y * 0.35, -Piece.Velocity.Z * 0.25);
					Piece.SpinRate *= 0.35f;
				}
				else
				{
					Piece.bLanded = true;
					Piece.Velocity = FVector::ZeroVector;
					Piece.SpinRate = 0.0f;
				}
			}
		}
		// Whole for the first part of its life, then it shrinks and sinks away smoothly to nothing at the end.
		const float Fade = FMath::SmoothStep(PlantBreakShrinkFrom * Life, Life, Piece.Age);
		const float Size = FMath::Max(0.001f, 1.0f - Fade);
		const FVector At = Piece.Location - FVector(0.0, 0.0, PlantBreakSinkCm * Fade);
		Piece.Mesh->SetWorldTransform(FTransform(Piece.Rotation, At, Piece.Scale * Size));
	}
}

#include "TreeFallSubsystem.h"

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInterface.h"

#include "ColonySim/Events.h"
#include "ColonySim/Map.h"
#include "ColonySim/Sim.h"

#include "BlastFxSubsystem.h"
#include "SimSubsystem.h"
#include "SoundSubsystem.h"
#include "TerrainSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> GTreeFallEnabled(
		TEXT("TreeFall.Enabled"), 1, TEXT("1: a tree the sim takes down topples over; 0: it just goes."), ECVF_Default);
	TAutoConsoleVariable<float> GTreeFallLieSeconds(
		TEXT("TreeFall.LieSeconds"), 25.0f, TEXT("Seconds a fallen tree lies on the ground before it sinks away."), ECVF_Default);
	TAutoConsoleVariable<float> GTreeFallSinkSeconds(
		TEXT("TreeFall.SinkSeconds"), 8.0f, TEXT("Seconds a fallen tree takes to sink out of sight."), ECVF_Default);
	TAutoConsoleVariable<float> GTreeFallRestitution(
		TEXT("TreeFall.Restitution"), 0.22f, TEXT("Share of its turning speed a falling tree keeps when it bounces off the ground."), ECVF_Default);
	TAutoConsoleVariable<float> GTreeFallCrownRest(
		TEXT("TreeFall.CrownRest"), 0.35f, TEXT("Share of the crown's half width the trunk rests above the ground on its branches."), ECVF_Default);
	TAutoConsoleVariable<int32> GTreeFallMax(
		TEXT("TreeFall.Max"), 24, TEXT("Most fallen trees lying about at once; the oldest sinks away first."), ECVF_Default);

	constexpr float TreeFallGravity = 9.81f;
	// A felled tree starts with the hinge just giving (a slight lean, barely moving); one broken by a blow, a shot or a
	// blast is pushed over harder.
	// (Under a degree and barely turning, so it leans slowly for a second or two and is down in about 3 to 4 s, like a real
	// felled tree; the first try started at 1.5 degrees and 0.04 rad/s and a poplar was down in about 2.5 s.)
	constexpr float TreeFallFelledLeanDeg = 0.8f;
	constexpr float TreeFallFelledRate = 0.015f;
	constexpr float TreeFallBrokenLeanDeg = 3.0f;
	constexpr float TreeFallBrokenRate = 0.25f;
	// Air on the crown takes a little of the speed (per second).
	constexpr float TreeFallAirDrag = 0.12f;
	// Below this turning speed (radians a second) a bounce is over and the tree lies still.
	constexpr float TreeFallSettleRate = 0.08f;
	constexpr int32 TreeFallMaxBounces = 3;
	// The farthest it can lean: on a slope falling downhill, a little past flat.
	constexpr float TreeFallMaxLeanDeg = 110.0f;
	constexpr float TreeFallSubstep = 1.0f / 120.0f;
}

bool UTreeFallSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UTreeFallSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UTreeFallSubsystem, STATGROUP_Tickables);
}

void UTreeFallSubsystem::Initialize(FSubsystemCollectionBase& InCollection)
{
	Super::Initialize(InCollection);
	if (USimSubsystem* Sim = InCollection.InitializeDependency<USimSubsystem>())
	{
		Bridge = Sim;
		EventHandle = Sim->OnSimEvent.AddUObject(this, &UTreeFallSubsystem::OnSimEvent);
	}
	InCollection.InitializeDependency<UTerrainSubsystem>();
	InCollection.InitializeDependency<UBlastFxSubsystem>();
	InCollection.InitializeDependency<USoundSubsystem>();
}

void UTreeFallSubsystem::Deinitialize()
{
	if (USimSubsystem* Sim = Bridge.Get())
	{
		Sim->OnSimEvent.Remove(EventHandle);
	}
	Teardown();
	Super::Deinitialize();
}

void UTreeFallSubsystem::EnsureScene()
{
	if (Holder || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("TreeFallRoot"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();
#if WITH_EDITOR
	Holder->SetActorLabel(TEXT("FallingTrees"));
#endif
}

void UTreeFallSubsystem::Teardown()
{
	for (FFall& Fall : Falls)
	{
		if (Fall.Tree.IsValid())
		{
			Fall.Tree->DestroyComponent();
		}
	}
	Falls.Reset();
	if (Holder)
	{
		Holder->Destroy();
		Holder = nullptr;
	}
}

int32 UTreeFallSubsystem::GetFalling() const
{
	int32 Count = 0;
	for (const FFall& Fall : Falls)
	{
		Count += Fall.Phase == EPhase::Falling ? 1 : 0;
	}
	return Count;
}

void UTreeFallSubsystem::PlayEventForTest(const colony::Event& Event)
{
	OnSimEvent(Event);
}

void UTreeFallSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type != colony::EventType::PlantRemoved || (Event.message != "felled" && Event.message != "broken") ||
		(Event.x == 0.0f && Event.y == 0.0f) || GTreeFallEnabled.GetValueOnGameThread() == 0)
	{
		return;
	}
	const USimSubsystem* Source = Bridge.Get();
	const UTerrainSubsystem* Terrain = GetWorld() ? GetWorld()->GetSubsystem<UTerrainSubsystem>() : nullptr;
	if (!Source || !Source->GetSim() || !Terrain)
	{
		return;
	}
	FTerrainPlantLook Look;
	if (!Terrain->GetPlantLook(Event.cell, Event.def, static_cast<uint32>(FMath::Max(Event.count, 0)), Look) || !Look.Mesh)
	{
		return;
	}
	EnsureScene();
	if (!Holder)
	{
		return;
	}

	// The same tree as the chunk drew, in the same place: the chunk drops it as this one takes over.
	UStaticMeshComponent* Tree = NewObject<UStaticMeshComponent>(Holder);
	Tree->SetStaticMesh(Look.Mesh);
	for (int32 Slot = 0; Slot < Look.Materials.Num(); ++Slot)
	{
		if (Look.Materials[Slot])
		{
			Tree->SetMaterial(Slot, Look.Materials[Slot]);
		}
	}
	Tree->SetMobility(EComponentMobility::Movable);
	Tree->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Tree->SetCanEverAffectNavigation(false);
	Tree->SetCastShadow(Look.bCastShadow);
	Tree->bAffectDistanceFieldLighting = false;
	if (Look.WindOffCm > 0.0f)
	{
		Tree->SetWorldPositionOffsetDisableDistance(static_cast<int32>(Look.WindOffCm));
	}
	Tree->SetupAttachment(Holder->GetRootComponent());
	Tree->RegisterComponent();
	Tree->SetWorldTransform(Look.Transform);

	FFall Fall;
	Fall.Tree = Tree;
	Fall.Standing = Look.Transform;
	Fall.Foot = Look.Transform.GetLocation();
	Fall.Dir = FVector(Event.x, Event.y, 0.0f).GetSafeNormal();
	if (Fall.Dir.IsNearlyZero())
	{
		Fall.Dir = FVector::XAxisVector;
	}
	Fall.Axis = FVector::CrossProduct(FVector::UpVector, Fall.Dir).GetSafeNormal();
	const FBox Bounds = Look.Mesh->GetBounds().GetBox();
	const FVector Scale = Look.Transform.GetScale3D();
	Fall.HeightM = FMath::Max(1.0f, static_cast<float>(Bounds.Max.Z * Scale.Z) / 100.0f);
	Fall.CrownCm = FMath::Max(30.0f, static_cast<float>(FMath::Max(Bounds.GetExtent().X * Scale.X, Bounds.GetExtent().Y * Scale.Y)));
	const bool bFelled = Event.message == "felled";
	Fall.Angle = FMath::DegreesToRadians(bFelled ? TreeFallFelledLeanDeg : TreeFallBrokenLeanDeg);
	Fall.Rate = (bFelled ? TreeFallFelledRate : TreeFallBrokenRate) * Random.FRandRange(0.85f, 1.15f);
	Fall.RestAngle = FindRestAngle(Fall);
	Falls.Add(Fall);
	++FallsStarted;

	if (USoundSubsystem* Sound = GetWorld()->GetSubsystem<USoundSubsystem>())
	{
		Sound->PlayCue(TEXT("Tree_Crack"), Fall.Foot + FVector(0.0, 0.0, 120.0));
	}

	// Within the budget: the oldest lying tree starts to sink.
	const int32 Budget = FMath::Max(1, GTreeFallMax.GetValueOnGameThread());
	int32 Lying = 0;
	for (int32 Index = Falls.Num() - 1; Index >= 0; --Index)
	{
		if (Falls[Index].Phase != EPhase::Falling && ++Lying > Budget && Falls[Index].Phase == EPhase::Lying)
		{
			Falls[Index].Phase = EPhase::Sinking;
			Falls[Index].Age = 0.0f;
		}
	}
}

float UTreeFallSubsystem::FindRestAngle(const FFall& Fall) const
{
	const USimSubsystem* Source = Bridge.Get();
	const colony::Sim* Sim = Source ? Source->GetSim() : nullptr;
	const float MaxLean = FMath::DegreesToRadians(TreeFallMaxLeanDeg);
	if (!Sim)
	{
		return FMath::DegreesToRadians(88.0f);
	}
	const colony::Map& Map = Sim->map();
	const FVector Origin = Source->GetWorldOrigin();
	const FVector FootM = (Fall.Foot - Origin) / 100.0;
	const float CrownM = Fall.CrownCm / 100.0f;
	const float CrownRest = FMath::Max(0.0f, GTreeFallCrownRest.GetValueOnGameThread());
	// What stands at a point of the map: the ground, or the top of a building there (a wall the tree falls onto).
	const auto SupportAt = [&Map](float X, float Y)
	{
		float Top = Map.heightAt(X, Y);
		const colony::CellPos Cell{FMath::FloorToInt32(X), FMath::FloorToInt32(Y), 0};
		if (Map.inBounds(Cell) && Map.buildingAt(Cell) != colony::kNoEntity)
		{
			const uint8 Dm = Map.obstacleDm(Cell);
			Top += Dm >= 255 ? 3.0f : static_cast<float>(Dm) * 0.1f;
		}
		return Top;
	};
	constexpr float Fractions[] = {0.2f, 0.35f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f};
	for (float Lean = FMath::DegreesToRadians(20.0f); Lean <= MaxLean; Lean += FMath::DegreesToRadians(0.5f))
	{
		const float Out = FMath::Sin(Lean);
		const float Up = FMath::Cos(Lean);
		for (const float F : Fractions)
		{
			const float R = F * Fall.HeightM;
			const float X = static_cast<float>(FootM.X + Fall.Dir.X * R * Out);
			const float Y = static_cast<float>(FootM.Y + Fall.Dir.Y * R * Out);
			const float Z = static_cast<float>(FootM.Z) + R * Up;
			// The crown's branches meet the ground before the trunk does: the higher up, the more they hold it off.
			const float Branches = CrownRest * CrownM * FMath::SmoothStep(0.4f, 0.85f, F);
			if (Z <= SupportAt(X, Y) + Branches)
			{
				return Lean;
			}
		}
	}
	return MaxLean;
}

void UTreeFallSubsystem::Place(FFall& Fall) const
{
	if (!Fall.Tree.IsValid())
	{
		return;
	}
	FTransform Now = Fall.Standing;
	Now.SetRotation(FQuat(Fall.Axis, Fall.Angle) * Fall.Standing.GetRotation());
	Now.SetLocation(Fall.Foot - FVector(0.0, 0.0, Fall.Sink));
	Fall.Tree->SetWorldTransform(Now);
}

void UTreeFallSubsystem::Land(FFall& Fall)
{
	++FallsLanded;
	const float Out = FMath::Sin(Fall.RestAngle);
	const float Up = FMath::Cos(Fall.RestAngle);
	const FVector Crown = Fall.Foot + (Fall.Dir * Out + FVector::UpVector * Up) * (Fall.HeightM * 100.0f * 0.75f);
	if (USoundSubsystem* Sound = GetWorld()->GetSubsystem<USoundSubsystem>())
	{
		Sound->PlayCue(TEXT("Tree_Fall"), Crown);
	}
	// Dust and leaf litter thrown up along the trunk and under the crown where it hits.
	UBlastFxSubsystem* Fx = GetWorld()->GetSubsystem<UBlastFxSubsystem>();
	if (Fx && Fx->IsReady())
	{
		const int32 Puffs = FMath::Clamp(FMath::RoundToInt32(Fall.HeightM * 0.8f), 3, 14);
		for (int32 I = 0; I < Puffs; ++I)
		{
			const float F = FMath::Lerp(0.35f, 1.0f, (static_cast<float>(I) + Random.FRand()) / static_cast<float>(Puffs));
			const FVector Along = Fall.Foot + (Fall.Dir * Out + FVector::UpVector * Up) * (Fall.HeightM * 100.0f * F);
			const FVector Side = Fall.Axis * Random.FRandRange(-0.6f, 0.6f) * Fall.CrownCm * F;
			Fx->AddDebrisTrail(Along + Side + FVector(0.0, 0.0, 20.0), FMath::Clamp(Fall.CrownCm * 0.35f, 60.0f, 160.0f) * Random.FRandRange(0.8f, 1.2f));
		}
	}
}

void UTreeFallSubsystem::Tick(float DeltaTime)
{
	USimSubsystem* Source = Bridge.Get();
	if (!Source || !Source->GetSim())
	{
		return;
	}
	if (BuiltGeneration != Source->GetSimGeneration())
	{
		// A new game or a load: the falling trees belong to another map.
		BuiltGeneration = Source->GetSimGeneration();
		Teardown();
	}
	if (Falls.IsEmpty() || Source->GetSpeed() <= 0)
	{
		return;  // paused: everything waits where it is
	}
	const float Dt = FMath::Min(DeltaTime, 0.1f);
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Dt / TreeFallSubstep));
	const float H = Dt / static_cast<float>(Steps);
	const float Restitution = FMath::Clamp(GTreeFallRestitution.GetValueOnGameThread(), 0.0f, 0.9f);
	const float LieSeconds = FMath::Max(0.0f, GTreeFallLieSeconds.GetValueOnGameThread());
	const float SinkSeconds = FMath::Max(0.5f, GTreeFallSinkSeconds.GetValueOnGameThread());
	for (int32 Index = Falls.Num() - 1; Index >= 0; --Index)
	{
		FFall& Fall = Falls[Index];
		if (!Fall.Tree.IsValid())
		{
			Falls.RemoveAt(Index);
			continue;
		}
		const float Before = Fall.Angle;
		switch (Fall.Phase)
		{
			case EPhase::Falling:
			{
				// A pole hinged at its foot: angular acceleration 3 g sin(lean) / 2 L.
				const float Pull = 1.5f * TreeFallGravity / Fall.HeightM;
				if (Fall.Bounces == 0)
				{
					Fall.Age += Dt;  // the time it takes to come down
				}
				for (int32 Step = 0; Step < Steps && Fall.Phase == EPhase::Falling; ++Step)
				{
					Fall.Rate += (Pull * FMath::Sin(Fall.Angle) - TreeFallAirDrag * Fall.Rate) * H;
					Fall.Angle += Fall.Rate * H;
					if (Fall.Angle < 0.0f)
					{
						Fall.Angle = 0.0f;
						Fall.Rate = FMath::Max(Fall.Rate, 0.0f);
					}
					if (Fall.Angle < Fall.RestAngle)
					{
						continue;
					}
					Fall.Angle = Fall.RestAngle;
					if (Fall.Bounces == 0)
					{
						LastFallSeconds = Fall.Age;
						Land(Fall);
					}
					++Fall.Bounces;
					Fall.Rate = -Fall.Rate * Restitution;
					if (FMath::Abs(Fall.Rate) < TreeFallSettleRate || Fall.Bounces >= TreeFallMaxBounces)
					{
						Fall.Rate = 0.0f;
						Fall.Phase = EPhase::Lying;
						Fall.Age = 0.0f;
					}
				}
				break;
			}
			case EPhase::Lying:
				Fall.Age += Dt;
				if (Fall.Age >= LieSeconds)
				{
					Fall.Phase = EPhase::Sinking;
					Fall.Age = 0.0f;
				}
				break;
			case EPhase::Sinking:
			{
				Fall.Age += Dt;
				const float Depth = Fall.CrownCm + 80.0f;
				Fall.Sink = Depth * FMath::SmoothStep(0.0f, 1.0f, FMath::Min(Fall.Age / SinkSeconds, 1.0f));
				if (Fall.Age >= SinkSeconds)
				{
					Fall.Tree->DestroyComponent();
					Falls.RemoveAt(Index);
					++FallsGone;
					continue;
				}
				break;
			}
		}
		BiggestStepDeg = FMath::Max(BiggestStepDeg, FMath::RadiansToDegrees(FMath::Abs(Fall.Angle - Before)));
		Place(Fall);
	}
}

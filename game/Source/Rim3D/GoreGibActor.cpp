#include "GoreGibActor.h"

#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "ProceduralMeshComponent.h"

#include "BloodSubsystem.h"
#include "GoreStump.h"
#include "SimSubsystem.h"

namespace
{
	TAutoConsoleVariable<int32> GGibCount(TEXT("Gore.GibCount"), 22,
		TEXT("How many chunks a corpse's trunk bursts into (body-health.md part F, \"Trunks burst\")."), ECVF_Default);
	TAutoConsoleVariable<float> GGibSeconds(TEXT("Gore.GibSeconds"), 300.0f,
		TEXT("How long a gib lies (real seconds) before it shrinks away; 300 matches the 5 minutes blood takes to fade."), ECVF_Default);
	TAutoConsoleVariable<int32> GGibMax(TEXT("Gore.GibMax"), 240,
		TEXT("At most this many gibs lie at once; the oldest go first."), ECVF_Default);
	TAutoConsoleVariable<float> GGibSpeed(TEXT("Gore.GibSpeed"), 1.0f,
		TEXT("How hard a burst throws its chunks (1: about 1.5 to 4 m/s, most land within 3 m)."), ECVF_Default);

	constexpr float GibGravity = 980.0f;
	constexpr float GibBounce = 0.22f;
	constexpr float GibGrip = 0.45f;       // the share of its sideways speed a chunk keeps at each bounce
	constexpr float GibRestSpeed = 30.0f;
	constexpr float GibShrinkSeconds = 3.0f;

	/** Every gib in play, oldest first (for the cap). */
	TArray<TWeakObjectPtr<AGoreGibActor>>& GibLive()
	{
		static TArray<TWeakObjectPtr<AGoreGibActor>> Live;
		return Live;
	}
}

AGoreGibActor::AGoreGibActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AGoreGibActor::Throw(const FVector& InVelocity, const FRotator& InSpin, float InRadius, bool bInSplat)
{
	Velocity = InVelocity;
	Spin = InSpin;
	Radius = InRadius;
	bSplat = bInSplat;
	bAtRest = false;
}

void AGoreGibActor::Push(const FVector& DeltaVelocity)
{
	if (DeltaVelocity.IsNearlyZero())
	{
		return;
	}
	Velocity += DeltaVelocity;
	Spin += FRotator(FMath::FRandRange(-360.0f, 360.0f), FMath::FRandRange(-360.0f, 360.0f), FMath::FRandRange(-360.0f, 360.0f));
	bAtRest = false;
}

int32 AGoreGibActor::CountLive(const UWorld* World)
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AGoreGibActor>& Gib : GibLive())
	{
		Count += Gib.IsValid() && Gib->GetWorld() == World ? 1 : 0;
	}
	return Count;
}

void AGoreGibActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GibLive().RemoveAll([this](const TWeakObjectPtr<AGoreGibActor>& Gib) { return !Gib.IsValid() || Gib.Get() == this; });
	Super::EndPlay(EndPlayReason);
}

void AGoreGibActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;
	const float Life = FMath::Max(GGibSeconds.GetValueOnGameThread(), 1.0f);
	if (Age >= Life)
	{
		// Gone as the blood goes: it shrinks into the ground.
		const float Left = 1.0f - (Age - Life) / GibShrinkSeconds;
		if (Left <= 0.0f)
		{
			Destroy();
			return;
		}
		SetActorScale3D(FVector(FMath::Max(Left, 0.01f)));
	}
	if (bAtRest)
	{
		return;
	}
	const USimSubsystem* Bridge = GetWorld() ? GetWorld()->GetSubsystem<USimSubsystem>() : nullptr;
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	const float Step = FMath::Min(DeltaSeconds, 0.05f);
	Velocity.Z -= GibGravity * Step;
	Velocity *= FMath::Max(0.0f, 1.0f - 0.25f * Step);  // a little drag in the air
	FVector At = GetActorLocation() + Velocity * Step;
	const FVector Here = (At - Bridge->GetWorldOrigin()) / 100.0;
	const float Floor = static_cast<float>(Bridge->SimToWorld(static_cast<float>(Here.X), static_cast<float>(Here.Y), 0).Z) + Radius;
	if (At.Z <= Floor)
	{
		At.Z = Floor;
		if (!bLanded)
		{
			bLanded = true;
			// Where it first slaps down it leaves a splat of blood.
			if (bSplat)
			{
				if (UBloodSubsystem* Blood = GetWorld()->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
				{
					FBloodHit Hit;
					Hit.At = At;
					Hit.Direction = FVector(Velocity.X * 0.3, Velocity.Y * 0.3, -200.0);
					Hit.Kind = EBloodWound::Blunt;
					Hit.Damage = 6.0f;
					Hit.Force = 0.35f;
					Blood->AddHit(Hit);
				}
			}
		}
		if (Velocity.Z < 0.0f)
		{
			Velocity.Z = -Velocity.Z * GibBounce;
		}
		Velocity.X *= GibGrip;
		Velocity.Y *= GibGrip;
		Spin = Spin * GibGrip;
		if (Velocity.Size() < GibRestSpeed)
		{
			bAtRest = true;
			Velocity = FVector::ZeroVector;
		}
	}
	const FQuat Turned = (GetActorQuat() * (Spin * Step).Quaternion()).GetNormalized();
	SetActorLocationAndRotation(At, Turned);
}

int32 AGoreGibActor::Burst(UWorld* World, const FVector& Centre, float BodyScale, float Strength, const FLinearColor& Skin, int32 Seed, colony::Entity Victim)
{
	if (!World)
	{
		return 0;
	}
	FRandomStream Random(Seed);
	const int32 Count = FMath::Clamp(GGibCount.GetValueOnGameThread(), 0, 64);
	const float Scale = FMath::Clamp(BodyScale, 0.5f, 1.5f);
	const float Speed = FMath::Clamp(Strength, 0.8f, 1.4f) * FMath::Max(GGibSpeed.GetValueOnGameThread(), 0.0f);
	// Room for them: the oldest gibs go first.
	TArray<TWeakObjectPtr<AGoreGibActor>>& Live = GibLive();
	Live.RemoveAll([](const TWeakObjectPtr<AGoreGibActor>& Gib) { return !Gib.IsValid(); });
	const int32 Max = FMath::Max(GGibMax.GetValueOnGameThread(), Count);
	while (Live.Num() + Count > Max && Live.Num() > 0)
	{
		const TWeakObjectPtr<AGoreGibActor> Old = Live[0];
		Live.RemoveAt(0);
		if (Old.IsValid())
		{
			Old->Destroy();
		}
	}
	int32 Made = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		// Mostly meat; a few bone shards and ribs; some dark wet lumps.
		const float Pick = static_cast<float>(i) / FMath::Max(Count, 1);
		const EGoreGibKind Kind = Pick < 0.5f ? EGoreGibKind::Meat : Pick < 0.67f ? EGoreGibKind::Rib : Pick < 0.83f ? EGoreGibKind::Bone : EGoreGibKind::Offal;
		float Size = 0.0f;
		switch (Kind)
		{
		// (PC round 1: at 5 to 13 cm they were lost in the grass.)
		case EGoreGibKind::Meat: Size = Random.FRandRange(11.0f, 22.0f); break;
		case EGoreGibKind::Rib: Size = Random.FRandRange(18.0f, 28.0f); break;
		case EGoreGibKind::Bone: Size = Random.FRandRange(11.0f, 18.0f); break;
		case EGoreGibKind::Offal: Size = Random.FRandRange(9.0f, 15.0f); break;
		}
		Size *= Scale;
		// Out from the trunk all round, a little up; the big ones don't go as far.
		FVector Dir = Random.GetUnitVector();
		Dir.Z = FMath::Abs(Dir.Z) * 0.8f + 0.25f;
		Dir.Normalize();
		const FVector Start = Centre + Dir * Random.FRandRange(4.0f, 14.0f) * Scale;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AGoreGibActor* Gib = World->SpawnActor<AGoreGibActor>(AGoreGibActor::StaticClass(), FTransform(Random.GetUnitVector().Rotation(), Start), Params);
		if (!Gib)
		{
			continue;
		}
		UProceduralMeshComponent* Mesh = GoreMakeGibChunk(Gib, Gib->GetRootComponent(), Kind, Size, Skin, Seed * 131 + i);
		if (!Mesh)
		{
			Gib->Destroy();
			continue;
		}
		Gib->SetChunk(Mesh);
		// (PC round 2: at 3 to 7 m/s, times up to 2, they flew 5 to 20 m and none lay near the body.) Most land within 1 to 3 m.
		const float Thrown = Random.FRandRange(150.0f, 380.0f) * Speed * FMath::Lerp(1.15f, 0.75f, FMath::Clamp((Size - 8.0f) / 20.0f, 0.0f, 1.0f));
		const FRotator GibSpin(Random.FRandRange(-900.0f, 900.0f), Random.FRandRange(-900.0f, 900.0f), Random.FRandRange(-900.0f, 900.0f));
		// A rib or shard rests on its side: its middle lies about its thickness over the ground.
		const float Rest = Kind == EGoreGibKind::Rib || Kind == EGoreGibKind::Bone ? Size * 0.08f : Size * 0.3f;
		Gib->Throw(Dir * Thrown, GibSpin, Rest, (i % 2) == 0);
		Live.Add(Gib);
		++Made;
	}
	// The burst itself: sprays of the body's blood all round (blast-like) and a heavy one down onto where it lay.
	if (UBloodSubsystem* Blood = World->GetSubsystem<UBloodSubsystem>(); Blood && Blood->IsReady())
	{
		for (int32 i = 0; i < 6; ++i)
		{
			FVector Dir = Random.GetUnitVector();
			Dir.Z = FMath::Abs(Dir.Z) * 0.5f;
			FBloodHit Hit;
			Hit.At = Centre;
			Hit.Direction = Dir;
			Hit.Kind = EBloodWound::Blast;
			Hit.Damage = 40.0f;
			Hit.Force = FMath::Clamp(1.4f * Speed, 0.8f, 2.5f);
			Hit.Victim = Victim;
			Hit.BleedSecondsWithoutSim = 1.0f;
			Blood->AddHit(Hit);
		}
		FBloodHit Pool;
		Pool.At = Centre;
		Pool.Direction = FVector(0.0, 0.0, -1.0);
		Pool.Kind = EBloodWound::Blunt;
		Pool.Damage = 60.0f;
		Pool.Force = 1.5f;
		Pool.Victim = Victim;
		Pool.BleedSecondsWithoutSim = 1.0f;
		Blood->AddHit(Pool);
	}
	UE_LOG(LogTemp, Log, TEXT("Gibs: a trunk burst into %d chunks at %s (strength %.2f)"), Made, *Centre.ToString(), Strength);
	return Made;
}

#include "CorpseLifeSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include "AnimalActor.h"
#include "AnimalSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "SimSubsystem.h"

#include "ColonySim/Snapshot.h"

namespace
{
	TAutoConsoleVariable<int32> CVarCorpseLife(TEXT("Corpse.Life"), 1, TEXT("Draw flies and maggots on dead bodies (0 off)."));
	TAutoConsoleVariable<int32> CVarCorpseMaxFlies(TEXT("Corpse.MaxFlies"), 28, TEXT("Flies on a full-sized body with a full swarm."));
	TAutoConsoleVariable<int32> CVarCorpseMaxMaggots(TEXT("Corpse.MaxMaggots"), 70, TEXT("Maggots on a full-sized body at their most."));
	TAutoConsoleVariable<float> CVarCorpseDistance(TEXT("Corpse.Distance"), 40.0f, TEXT("Bodies further than this (metres) from the camera get no flies or maggots."));
	TAutoConsoleVariable<int32> CVarCorpseDebug(TEXT("Corpse.Debug"), 0, TEXT("Log the bodies, flies and maggots drawn once a second."));

	const TCHAR* const CorpseFlyPath = TEXT("/Game/Rim3D/Generated/Insects/SM_HouseFly.SM_HouseFly");
	const TCHAR* const CorpseSpherePath = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	const TCHAR* const CorpseShapeMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	// A house fly is about 7 mm long; the engine sphere is 100 cm across.
	constexpr float CorpseFlyLengthCm = 0.7f;
	// A blowfly maggot near the end of its growth is about 1.5 cm.
	constexpr float CorpseMaggotLengthCm = 1.4f;

	uint32 CorpseKey(colony::Entity Thing)
	{
		return static_cast<uint32>(entt::to_integral(Thing));
	}

	// A random point on the upper half of an ellipsoid round the body: where a fly sits or a maggot crawls.
	FVector CorpseSurfacePoint(FRandomStream& R, const FVector& Extent)
	{
		const float A = R.FRandRange(0.0f, 2.0f * PI);
		const float Rad = FMath::Sqrt(R.FRand());
		const float X = FMath::Cos(A) * Rad;
		const float Y = FMath::Sin(A) * Rad;
		const float Z = FMath::Sqrt(FMath::Max(0.0f, 1.0f - X * X - Y * Y));
		return FVector(X * Extent.X, Y * Extent.Y, Z * Extent.Z);
	}
}

bool UCorpseLifeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UCorpseLifeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<USimSubsystem>();
	Rand.Initialize(0xF11E5);
}

void UCorpseLifeSubsystem::Deinitialize()
{
	Clear();
	if (IsValid(Holder))
	{
		Holder->Destroy();
	}
	Holder = nullptr;
	Super::Deinitialize();
}

TStatId UCorpseLifeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCorpseLifeSubsystem, STATGROUP_Tickables);
}

bool UCorpseLifeSubsystem::EnsureComponents()
{
	if (IsValid(FlyMeshes) && IsValid(MaggotMeshes))
	{
		return true;
	}
	if (bTriedComponents)
	{
		return false;
	}
	bTriedComponents = true;
	UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, CorpseSpherePath);
	UStaticMesh* Fly = LoadObject<UStaticMesh>(nullptr, CorpseFlyPath);
	bRealFly = Fly != nullptr;
	if (Fly)
	{
		// The art table imports it at whatever size the file has: scale it to a real house fly's length.
		const FVector Size = Fly->GetBoundingBox().GetSize();
		FlyScale = CorpseFlyLengthCm / static_cast<float>(FMath::Max(1e-3, Size.GetMax()));
	}
	if (!Fly)
	{
		Fly = Sphere;
		UE_LOG(LogTemp, Log, TEXT("Corpse life: no %s yet (AnimalArt commandlet), flies are dark blobs"), CorpseFlyPath);
	}
	if (!Sphere || !Fly)
	{
		UE_LOG(LogTemp, Warning, TEXT("Corpse life: engine sphere missing, no flies or maggots"));
		return false;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;
	Holder = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (!Holder)
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
	Holder->SetRootComponent(Root);
	Root->RegisterComponent();

	auto MakeIsm = [&](const TCHAR* Name, UStaticMesh* Mesh) {
		UInstancedStaticMeshComponent* Ism = NewObject<UInstancedStaticMeshComponent>(Holder, Name);
		Ism->SetStaticMesh(Mesh);
		Ism->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ism->SetCastShadow(false);
		Ism->SetMobility(EComponentMobility::Movable);
		Ism->SetupAttachment(Root);
		Ism->RegisterComponent();
		return Ism;
	};
	FlyMeshes = MakeIsm(TEXT("Flies"), Fly);
	MaggotMeshes = MakeIsm(TEXT("Maggots"), Sphere);

	if (UMaterialInterface* Shape = LoadObject<UMaterialInterface>(nullptr, CorpseShapeMaterialPath))
	{
		if (!bRealFly)
		{
			FlyMaterial = UMaterialInstanceDynamic::Create(Shape, this);
			FlyMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.025f, 0.02f));
			FlyMeshes->SetMaterial(0, FlyMaterial);
		}
		MaggotMaterial = UMaterialInstanceDynamic::Create(Shape, this);
		MaggotMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.78f, 0.72f, 0.58f));
		MaggotMeshes->SetMaterial(0, MaggotMaterial);
	}
	return true;
}

void UCorpseLifeSubsystem::Clear()
{
	Bodies.Reset();
	if (IsValid(FlyMeshes))
	{
		FlyMeshes->ClearInstances();
	}
	if (IsValid(MaggotMeshes))
	{
		MaggotMeshes->ClearInstances();
	}
}

bool UCorpseLifeSubsystem::BodyBounds(colony::Entity Thing, const FVector& Fallback, FVector& Centre, FVector& Extent) const
{
	const USkeletalMeshComponent* Mesh = nullptr;
	if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
	{
		if (const ASimPawnActor* Pawn = Pawns->FindPawn(Thing))
		{
			Mesh = Pawn->GetDecalMesh();
		}
	}
	if (!Mesh)
	{
		if (const USimAnimalSubsystem* Animals = GetWorld()->GetSubsystem<USimAnimalSubsystem>())
		{
			if (const ASimAnimalActor* Animal = Animals->FindAnimal(Thing))
			{
				Mesh = Animal->GetBody();
			}
		}
	}
	if (Mesh && Mesh->IsRegistered())
	{
		const FBoxSphereBounds B = Mesh->Bounds;
		Centre = B.Origin;
		// A body lying down: its top half is where flies sit; keep the box flat-ish and not too small.
		Extent = FVector(FMath::Max(B.BoxExtent.X, 8.0f), FMath::Max(B.BoxExtent.Y, 8.0f), FMath::Clamp(B.BoxExtent.Z, 4.0f, 40.0f));
		return true;
	}
	Centre = Fallback + FVector(0.0f, 0.0f, 10.0f);
	Extent = FVector(60.0f, 25.0f, 12.0f);
	return false;
}

void UCorpseLifeSubsystem::StepFlies(FBody& Body, int32 Want, float FlyingShare, float Dt)
{
	FRandomStream R(Body.Seed + static_cast<uint32>(Body.Flies.Num()));
	while (Body.Flies.Num() < Want)
	{
		FFly F;
		F.Seat = CorpseSurfacePoint(R, Body.Extent);
		F.Pos = F.Seat;
		F.Switch = R.FRandRange(0.5f, 6.0f);
		Body.Flies.Add(F);
	}
	if (Body.Flies.Num() > Want)
	{
		Body.Flies.SetNum(Want);
	}
	const float Radius = Body.Extent.Size2D() * 1.25f + 15.0f;
	for (FFly& F : Body.Flies)
	{
		F.Switch -= Dt;
		if (F.Switch <= 0.0f)
		{
			// Take off with the flying share's chance, else land on a new spot.
			F.bFlying = Rand.FRand() < FlyingShare;
			F.Switch = F.bFlying ? Rand.FRandRange(1.5f, 7.0f) : Rand.FRandRange(1.0f, 5.0f);
			if (!F.bFlying)
			{
				F.Seat = CorpseSurfacePoint(Rand, Body.Extent);
			}
		}
		if (F.bFlying)
		{
			F.Retarget -= Dt;
			if (F.Retarget <= 0.0f)
			{
				const FVector Dir = Rand.GetUnitVector();
				F.Goal = FVector(Dir.X * Radius, Dir.Y * Radius, FMath::Abs(Dir.Z) * Radius * 0.6f + 5.0f);
				F.Retarget = Rand.FRandRange(0.15f, 0.6f);
			}
			// Quick, jerky darts: steer hard at the goal, cap the speed (a house fly does about 2 m/s).
			const FVector Steer = (F.Goal - F.Pos).GetSafeNormal() * 2500.0f;
			F.Vel = (F.Vel + Steer * Dt).GetClampedToMaxSize(220.0f);
			F.Pos += F.Vel * Dt;
		}
		else
		{
			// Fly in and sit; a small shuffle while sitting.
			const FVector To = F.Seat - F.Pos;
			if (To.SizeSquared() > 4.0f)
			{
				F.Vel = To.GetSafeNormal() * FMath::Min(150.0f, To.Size() * 6.0f);
				F.Pos += F.Vel * Dt;
			}
			else
			{
				F.Vel = FVector::ZeroVector;
				F.Pos = F.Seat + FVector(Rand.FRandRange(-0.05f, 0.05f), Rand.FRandRange(-0.05f, 0.05f), 0.0f);
			}
		}
		const FVector Facing = F.Vel.SizeSquared() > 1.0f ? F.Vel : FVector(F.Seat.X, F.Seat.Y, 0.0f);
		const FRotator Rot = Facing.IsNearlyZero() ? FRotator::ZeroRotator : Facing.Rotation();
		const FVector Scale = bRealFly ? FVector(FlyScale) : FVector(CorpseFlyLengthCm / 100.0f, CorpseFlyLengthCm * 0.55f / 100.0f, CorpseFlyLengthCm * 0.5f / 100.0f);
		FlyXforms.Emplace(Rot, Body.Centre + F.Pos, Scale);
	}
}

void UCorpseLifeSubsystem::StepMaggots(FBody& Body, int32 Want, float Dt)
{
	FRandomStream R(Body.Seed * 7u + static_cast<uint32>(Body.Maggots.Num()));
	while (Body.Maggots.Num() < Want)
	{
		FMaggot M;
		// Maggots gather in masses: most near a few spots on the body.
		const FVector Spot = CorpseSurfacePoint(R, Body.Extent * 0.7f);
		M.Pos = Spot + FVector(R.FRandRange(-6.0f, 6.0f), R.FRandRange(-6.0f, 6.0f), 0.0f);
		M.Heading = R.FRandRange(0.0f, 360.0f);
		M.Phase = R.FRandRange(0.0f, 2.0f * PI);
		M.Size = R.FRandRange(0.6f, 1.15f);
		Body.Maggots.Add(M);
	}
	if (Body.Maggots.Num() > Want)
	{
		Body.Maggots.SetNum(Want);
	}
	for (FMaggot& M : Body.Maggots)
	{
		M.Phase += Dt * 5.0f;
		M.Heading += Rand.FRandRange(-60.0f, 60.0f) * Dt;
		// About 3 mm a second, in pulses.
		const float Speed = 0.3f * (0.5f + 0.5f * FMath::Sin(M.Phase));
		const FVector Fwd = FRotator(0.0f, M.Heading, 0.0f).Vector();
		M.Pos += Fwd * Speed * Dt;
		// Stay on the top of the body.
		const float Nx = M.Pos.X / Body.Extent.X;
		const float Ny = M.Pos.Y / Body.Extent.Y;
		const float Rr = Nx * Nx + Ny * Ny;
		if (Rr > 0.8f)
		{
			M.Heading += 180.0f;
			M.Pos *= 0.97f;
		}
		M.Pos.Z = FMath::Sqrt(FMath::Max(0.0f, 1.0f - FMath::Min(Rr, 1.0f))) * Body.Extent.Z;
		const float Squirm = 1.0f + 0.18f * FMath::Sin(M.Phase * 1.7f);
		const float L = CorpseMaggotLengthCm * M.Size / 100.0f;
		MaggotXforms.Emplace(FRotator(0.0f, M.Heading, 0.0f), Body.Centre + M.Pos + FVector(0, 0, 0.3f),
			FVector(L * Squirm, L * 0.38f / Squirm, L * 0.3f));
	}
}

void UCorpseLifeSubsystem::Tick(float DeltaTime)
{
	++Frame;
	BodiesDrawn = FliesDrawn = MaggotsDrawn = 0;
	USimSubsystem* Bridge = GetWorld()->GetSubsystem<USimSubsystem>();
	if (!Bridge || !Bridge->GetSim() || CVarCorpseLife.GetValueOnGameThread() == 0)
	{
		Clear();
		return;
	}
	if (Bridge->GetSim() != LastSim)
	{
		Clear();
		LastSim = Bridge->GetSim();
	}
	FVector Camera = FVector::ZeroVector;
	bool bCamera = false;
	if (const APlayerController* PC = GetWorld()->GetFirstPlayerController(); PC && PC->PlayerCameraManager)
	{
		Camera = PC->PlayerCameraManager->GetCameraLocation();
		bCamera = true;
	}
	const float MaxDist = CVarCorpseDistance.GetValueOnGameThread() * 100.0f;
	const int32 MaxFlies = FMath::Max(0, CVarCorpseMaxFlies.GetValueOnGameThread());
	const int32 MaxMaggots = FMath::Max(0, CVarCorpseMaxMaggots.GetValueOnGameThread());
	const float Dt = FMath::Min(DeltaTime, 0.1f);  // real time: flies keep their pace at any game speed

	FlyXforms.Reset();
	MaggotXforms.Reset();
	const colony::Snapshot& Now = Bridge->GetCurrentSnapshot();
	for (const colony::ThingView& View : Now.things)
	{
		if (!View.dead || (View.flies <= 0.0f && View.maggots <= 0.0f))
		{
			continue;
		}
		FVector Location;
		if (!Bridge->GetBlendedLocation(View.id, Location))
		{
			Location = Bridge->SimToWorld(View.x, View.y, View.level);
		}
		if (bCamera && FVector::DistSquared(Location, Camera) > MaxDist * MaxDist)
		{
			continue;
		}
		if (!EnsureComponents())
		{
			return;
		}
		const uint32 Key = CorpseKey(View.id);
		FBody& Body = Bodies.FindOrAdd(Key);
		if (Body.Seed == 0)
		{
			Body.Seed = Key * 2654435761u + 1u;
		}
		Body.SeenFrame = Frame;
		BodyBounds(View.id, Location, Body.Centre, Body.Extent);
		// A human lying down is about 170 x 50 cm from above: size the swarm by the area the body covers.
		const float SizeShare = FMath::Clamp((Body.Extent.X * Body.Extent.Y) / (85.0f * 25.0f), 0.15f, 2.0f);
		StepFlies(Body, FMath::RoundToInt(View.flies * MaxFlies * SizeShare), View.fliesFlying, Dt);
		StepMaggots(Body, FMath::RoundToInt(View.maggots * MaxMaggots * SizeShare), Dt);
		++BodiesDrawn;
	}
	for (auto It = Bodies.CreateIterator(); It; ++It)
	{
		if (It.Value().SeenFrame != Frame)
		{
			It.RemoveCurrent();
		}
	}
	FliesDrawn = FlyXforms.Num();
	MaggotsDrawn = MaggotXforms.Num();
	if (IsValid(FlyMeshes))
	{
		FlyMeshes->ClearInstances();
		FlyMeshes->AddInstances(FlyXforms, false, true);
	}
	if (IsValid(MaggotMeshes))
	{
		MaggotMeshes->ClearInstances();
		MaggotMeshes->AddInstances(MaggotXforms, false, true);
	}
	if (CVarCorpseDebug.GetValueOnGameThread() != 0 && Frame % 60 == 0)
	{
		UE_LOG(LogTemp, Log, TEXT("Corpse life: %d bodies, %d flies (%s), %d maggots"), BodiesDrawn, FliesDrawn,
			bRealFly ? TEXT("house fly") : TEXT("blobs"), MaggotsDrawn);
	}
}

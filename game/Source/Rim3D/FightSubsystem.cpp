#include "FightSubsystem.h"

#include "Camera/PlayerCameraManager.h"
#include "Components/DecalComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

#include <imgui.h>

#include "OrdersSubsystem.h"
#include "PawnActor.h"
#include "PawnSubsystem.h"
#include "ProtagonistPawn.h"
#include "ProtagonistSubsystem.h"
#include "SimSubsystem.h"
#include "VisualMesh.h"
#include "WeaponDrawSubsystem.h"

#include "ColonySim/Clock.h"
#include "ColonySim/Colonist.h"
#include "ColonySim/Combat.h"
#include "ColonySim/Commands.h"
#include "ColonySim/Cover.h"
#include "ColonySim/Explosives.h"
#include "ColonySim/Defs.h"
#include "ColonySim/Sim.h"
#include "ColonySim/Snapshot.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFights, Log, All);

namespace
{
	TAutoConsoleVariable<int32> GFightEffects(
		TEXT("Fight.Effects"), 1, TEXT("1 draws tracers, muzzle flashes, impacts, wounds and ragdolls for the sim's fights, 0 draws none."), ECVF_Default);
	TAutoConsoleVariable<int32> GFightHitboxes(
		TEXT("Fight.Hitboxes"), 0, TEXT("1 draws every pawn's hitboxes where the sim has them (colour by body part)."), ECVF_Default);

	const TCHAR* const FightSolidMaterialPath = TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial");
	const TCHAR* const FightDecalMaterialPath = TEXT("/Game/Rim3D/Generated/Fights/M_WoundDecal.M_WoundDecal");

	// How far the crosshair reaches, metres.
	constexpr float FightAimRangeM = 150.0f;
	// A gun lying this close (metres) is picked up by E.
	constexpr float FightEquipRangeM = 8.0f;
	// Bullets that never report a hit are dropped after this long (real seconds) or this far (cm).
	constexpr float FightTracerMaxAge = 6.0f;
	constexpr float FightTracerMaxDistance = 20000.0f;
	constexpr int32 FightMaxTracers = 400;
	constexpr int32 FightMaxParticles = 1800;
	constexpr int32 FightMaxDecals = 160;
	constexpr int32 FightLightCount = 6;
	constexpr float FightLightSeconds = 0.09f;
	constexpr float FightLightIntensity = 2500.0f;
	// Cm per second of velocity change a hit gives a falling pawn, per point of damage.
	constexpr float FightImpulsePerDamage = 25.0f;
	constexpr float FightImpulseMax = 700.0f;

	struct FFightParticleLook
	{
		FLinearColor Color;
		bool bSphere;
	};

	FFightParticleLook FightParticleLook(EFightParticle Kind)
	{
		switch (Kind)
		{
			case EFightParticle::Dust: return {FLinearColor(0.42f, 0.34f, 0.24f), false};
			case EFightParticle::Chip: return {FLinearColor(0.32f, 0.32f, 0.34f), false};
			case EFightParticle::Spark: return {FLinearColor(1.0f, 0.62f, 0.18f), false};
			case EFightParticle::Blood: return {FLinearColor(0.55f, 0.02f, 0.02f), false};
			case EFightParticle::Smoke: return {FLinearColor(0.55f, 0.55f, 0.58f), true};
			default: return {FLinearColor(1.0f, 0.92f, 0.62f), true};
		}
	}

	FVector FightSimToWorldUnits(const USimSubsystem& Bridge, float X, float Y, float Z)
	{
		return Bridge.GetWorldOrigin() + FVector(X * 100.0, Y * 100.0, Z * 100.0);
	}

	/** Cm from the point (sim metres) to a hitbox shape (0 on or inside it). */
	float FightHitboxDistanceCm(const colony::HitboxView& H, float X, float Y, float Z)
	{
		const FVector P(X, Y, Z);
		if (!H.box)
		{
			const FVector A(H.ax, H.ay, H.az);
			const FVector AB = FVector(H.bx, H.by, H.bz) - A;
			const double LengthSquared = AB.SizeSquared();
			const double T = LengthSquared > 1.0e-8 ? FMath::Clamp(FVector::DotProduct(P - A, AB) / LengthSquared, 0.0, 1.0) : 0.0;
			return static_cast<float>(FMath::Max(0.0, FVector::Dist(P, A + AB * T) - H.radius) * 100.0);
		}
		const FVector Forward(H.forwardX, H.forwardY, 0.0);
		const FVector Left(-H.forwardY, H.forwardX, 0.0);
		const FVector D = P - FVector(H.ax, H.ay, H.az);
		const FVector Local(FVector::DotProduct(D, Forward), FVector::DotProduct(D, Left), D.Z);
		const FVector Outside(FMath::Max(0.0, FMath::Abs(Local.X) - H.bx), FMath::Max(0.0, FMath::Abs(Local.Y) - H.by), FMath::Max(0.0, FMath::Abs(Local.Z) - H.bz));
		return static_cast<float>(Outside.Size() * 100.0);
	}

	void FightText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		ImGui::TextUnformatted(Utf8.Get(), Utf8.Get() + Utf8.Length());
	}
}

bool UFightSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId UFightSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFightSubsystem, STATGROUP_Tickables);
}

void UFightSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (USimSubsystem* Bridge = Collection.InitializeDependency<USimSubsystem>())
	{
		BridgeSource = Bridge;
		EventHandle = Bridge->OnSimEvent.AddUObject(this, &UFightSubsystem::OnSimEvent);
	}
	Collection.InitializeDependency<USimPawnSubsystem>();
	Collection.InitializeDependency<UProtagonistSubsystem>();
}

void UFightSubsystem::Deinitialize()
{
	if (USimSubsystem* Bridge = BridgeSource.Get())
	{
		Bridge->OnSimEvent.Remove(EventHandle);
	}
	Super::Deinitialize();
}

bool UFightSubsystem::EffectsOn() const
{
	return GFightEffects.GetValueOnGameThread() != 0;
}

// ---------------------------------------------------------------------------
// The things the effects are drawn with.

UInstancedStaticMeshComponent* UFightSubsystem::MakeInstanced(UStaticMesh* Mesh)
{
	UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Holder);
	Component->SetStaticMesh(Mesh);
	Component->SetMobility(EComponentMobility::Movable);
	Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Component->SetGenerateOverlapEvents(false);
	Component->SetCanEverAffectNavigation(false);
	Component->SetCastShadow(false);
	Component->bReceivesDecals = false;
	Component->bUseAsOccluder = false;
	Component->SetupAttachment(Holder->GetRootComponent());
	Component->RegisterComponent();
	return Component;
}

void UFightSubsystem::EnsureEffects()
{
	if (Holder && SolidMaterial)
	{
		return;
	}
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	if (!Holder)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
		USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("Root"));
		Holder->SetRootComponent(Root);
		Root->RegisterComponent();
	}
	SolidMaterial = LoadObject<UMaterialInterface>(nullptr, FightSolidMaterialPath);
	if (!SolidMaterial)
	{
		UE_LOG(LogFights, Error, TEXT("Could not load %s; fight effects cannot be drawn"), FightSolidMaterialPath);
		return;
	}

	const auto MakeBox = [this](float X, float Y, float Z, const FLinearColor& Color, bool bSphere)
	{
		VisualMesh::FPart Part;
		Part.Shape = bSphere ? VisualMesh::EShape::Sphere : VisualMesh::EShape::Box;
		Part.Size = FVector3f(X, Y, Z);
		Part.Color = Color;
		TArray<VisualMesh::FPart> Parts;
		Parts.Add(Part);
		UStaticMesh* Mesh = VisualMesh::MakeMesh(this, SolidMaterial, Parts, VisualMesh::EPartSet::All, nullptr, false);
		if (Mesh)
		{
			KeepMeshes.Add(Mesh);
		}
		return Mesh;
	};

	// A tracer is a long thin box standing on z = 0, stretched to length per instance.
	TracerInstances = MakeInstanced(MakeBox(1.6f, 1.6f, 100.0f, FLinearColor(1.0f, 0.78f, 0.3f), false));
	ParticleInstances.Reset();
	for (int32 Kind = 0; Kind < static_cast<int32>(EFightParticle::Count); ++Kind)
	{
		const FFightParticleLook Look = FightParticleLook(static_cast<EFightParticle>(Kind));
		ParticleInstances.Add(MakeInstanced(Look.bSphere ? MakeBox(5.0f, 5.0f, 10.0f, Look.Color, true) : MakeBox(10.0f, 10.0f, 10.0f, Look.Color, false)));
	}

	Lights.Reset();
	LightAges.Reset();
	for (int32 I = 0; I < FightLightCount; ++I)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Holder);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetIntensity(0.0f);
		Light->SetLightColor(FLinearColor(1.0f, 0.72f, 0.38f));
		Light->SetAttenuationRadius(700.0f);
		Light->SetCastShadows(false);
		Light->SetupAttachment(Holder->GetRootComponent());
		Light->RegisterComponent();
		Light->SetVisibility(false);
		Lights.Add(Light);
		LightAges.Add(FightLightSeconds);
	}
}

void UFightSubsystem::ResetEffects()
{
	Tracers.Reset();
	Particles.Reset();
	RecentImpulse.Reset();
	HitLog.Reset();
	for (TWeakObjectPtr<UDecalComponent>& Decal : Decals)
	{
		if (Decal.IsValid())
		{
			Decal->DestroyComponent();
		}
	}
	Decals.Reset();
	if (TracerInstances)
	{
		TracerInstances->ClearInstances();
	}
	for (UInstancedStaticMeshComponent* Component : ParticleInstances)
	{
		if (Component)
		{
			Component->ClearInstances();
		}
	}
	for (UPointLightComponent* Light : Lights)
	{
		if (Light)
		{
			Light->SetVisibility(false);
		}
	}
}

// ---------------------------------------------------------------------------
// Sim events.

void UFightSubsystem::OnSimEvent(const colony::Event& Event)
{
	if (Event.type == colony::EventType::ShotFired)
	{
		OnShotFired(Event);
	}
	else if (Event.type == colony::EventType::ProjectileHit)
	{
		OnProjectileHit(Event);
	}
	else if (Event.type == colony::EventType::PawnDowned || Event.type == colony::EventType::PawnDied || Event.type == colony::EventType::PawnPartLost)
	{
		// The hit that did this was checked against hitboxes that are already gone or lying down: not a fair check.
		const USimSubsystem* Bridge = BridgeSource.Get();
		for (FFightHitRecord& Record : HitLog)
		{
			if (Bridge && Record.Pawn == Event.thing && Record.GameSeconds == Bridge->GetGameSeconds())
			{
				Record.MissCm = -1.0f;
			}
		}
	}
}

void UFightSubsystem::OnShotFired(const colony::Event& Event)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	++ShotsSeen;
	if (!Bridge || !EffectsOn())
	{
		return;
	}
	EnsureEffects();
	const FVector Muzzle = FightSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z);
	SpawnFlash(Muzzle);
	if (Tracers.Num() < FightMaxTracers)
	{
		FFightTracer Tracer;
		Tracer.Id = Event.target;
		Tracer.Shooter = Event.thing;
		Tracer.Start = Muzzle;
		Tracers.Add(Tracer);
		++TracersSpawned;
	}
}

void UFightSubsystem::OnProjectileHit(const colony::Event& Event)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	++HitsSeen;
	const bool bOnPawn = Event.hit == colony::ProjectileHitKind::Pawn || Event.hit == colony::ProjectileHitKind::Armor;
	if (bOnPawn)
	{
		// Does the hit point lie on the body the sim says was hit? (the hitbox debug view's number)
		++PawnHitsSeen;
		FFightHitRecord Record;
		Record.Pawn = Event.target;
		Record.Kind = Event.hit;
		Record.Part = Event.part;
		Record.Amount = Event.amount;
		Record.Where = FVector(Event.x, Event.y, Event.z);
		Record.GameSeconds = Bridge->GetGameSeconds();
		Record.MissCm = DistanceToHitboxesCm(Event.target, Event.x, Event.y, Event.z);
		if (HitLog.Num() < 512)
		{
			HitLog.Add(Record);
		}
	}
	if (!EffectsOn())
	{
		return;
	}
	EnsureEffects();

	const FVector Where = FightSimToWorldUnits(*Bridge, Event.x, Event.y, Event.z);
	for (FFightTracer& Tracer : Tracers)
	{
		if (Tracer.Id == Event.thing)
		{
			Tracer.bEnded = true;
			Tracer.End = Where;
			Tracer.HitKind = Event.hit;
			Tracer.Target = Event.target;
			Tracer.Part = Event.part;
			Tracer.Amount = Event.amount;
			return;
		}
	}
	// A bullet nobody drew (a loaded game, too many tracers): the impact happens at once.
	FFightTracer Immediate;
	Immediate.Id = Event.thing;
	Immediate.Start = Where;
	Immediate.Dir = -FVector::UpVector;
	Immediate.End = Where;
	Immediate.bEnded = true;
	Immediate.HitKind = Event.hit;
	Immediate.Target = Event.target;
	Immediate.Part = Event.part;
	Immediate.Amount = Event.amount;
	SpawnImpact(Immediate);
}

float UFightSubsystem::DistanceToHitboxesCm(colony::Entity Pawn, float X, float Y, float Z) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return -1.0f;
	}
	std::vector<colony::HitboxView> Boxes;
	if (!Bridge->GetSim()->combat().hitboxesOf(Pawn, Boxes) || Boxes.empty())
	{
		return -1.0f;
	}
	float Best = TNumericLimits<float>::Max();
	for (const colony::HitboxView& Box : Boxes)
	{
		Best = FMath::Min(Best, FightHitboxDistanceCm(Box, X, Y, Z));
	}
	return Best;
}

// ---------------------------------------------------------------------------
// Making effects.

void UFightSubsystem::SpawnBurst(EFightParticle Kind, const FVector& At, const FVector& Direction, int32 Count, float Speed, float Spread, float SizeCm, float Life, float Gravity)
{
	for (int32 I = 0; I < Count && Particles.Num() < FightMaxParticles; ++I)
	{
		FParticle Particle;
		Particle.Kind = Kind;
		Particle.Position = At;
		Particle.Velocity = (Direction.GetSafeNormal() + Random.VRand() * Spread).GetSafeNormal() * Speed * Random.FRandRange(0.4f, 1.0f);
		Particle.SizeCm = SizeCm * Random.FRandRange(0.6f, 1.2f);
		Particle.Life = Life * Random.FRandRange(0.6f, 1.1f);
		Particle.Gravity = Gravity;
		Particle.Drag = Kind == EFightParticle::Smoke ? 1.5f : 0.6f;
		Particles.Add(Particle);
	}
}

void UFightSubsystem::SpawnFlash(const FVector& Muzzle)
{
	++Flashes;
	if (Particles.Num() + 4 < FightMaxParticles)
	{
		FParticle Big;
		Big.Kind = EFightParticle::Flash;
		Big.Position = Muzzle;
		Big.SizeCm = 22.0f;
		Big.Life = 0.05f;
		Big.Gravity = 0.0f;
		Particles.Add(Big);
		FParticle Small = Big;
		Small.SizeCm = 11.0f;
		Small.Life = 0.1f;
		Particles.Add(Small);
	}
	SpawnBurst(EFightParticle::Smoke, Muzzle, FVector::UpVector, 2, 70.0f, 0.5f, 7.0f, 0.8f, -45.0f);
	if (Lights.Num() > 0)
	{
		const int32 Index = NextLight++ % Lights.Num();
		if (Lights[Index])
		{
			Lights[Index]->SetWorldLocation(Muzzle);
			Lights[Index]->SetVisibility(true);
			Lights[Index]->SetIntensity(FightLightIntensity);
			LightAges[Index] = 0.0f;
		}
	}
}

void UFightSubsystem::SpawnImpact(const FFightTracer& Tracer)
{
	++Impacts;
	++ImpactsByKind[FMath::Clamp(static_cast<int32>(Tracer.HitKind), 0, 7)];
	const FVector At = Tracer.End;
	const FVector Dir = Tracer.Dir.GetSafeNormal().IsNearlyZero() ? -FVector::UpVector : Tracer.Dir.GetSafeNormal();
	const FVector Back = -Dir;
	switch (Tracer.HitKind)
	{
		case colony::ProjectileHitKind::Ground:
			SpawnBurst(EFightParticle::Dust, At, FVector::UpVector, 9, 300.0f, 0.8f, 8.0f, 0.9f, 980.0f);
			SpawnBurst(EFightParticle::Smoke, At, FVector::UpVector, 2, 60.0f, 0.4f, 12.0f, 1.0f, -30.0f);
			break;
		case colony::ProjectileHitKind::Rock:
			SpawnBurst(EFightParticle::Dust, At, Back + FVector::UpVector * 0.5f, 6, 280.0f, 0.8f, 8.0f, 0.8f, 980.0f);
			SpawnBurst(EFightParticle::Chip, At, Back + FVector::UpVector * 0.5f, 5, 380.0f, 0.7f, 5.0f, 0.7f, 980.0f);
			SpawnBurst(EFightParticle::Spark, At, Back, 3, 450.0f, 0.6f, 2.5f, 0.3f, 980.0f);
			break;
		case colony::ProjectileHitKind::Building:
			SpawnBurst(EFightParticle::Spark, At, Back + FVector::UpVector * 0.3f, 8, 480.0f, 0.7f, 2.5f, 0.35f, 980.0f);
			SpawnBurst(EFightParticle::Chip, At, Back + FVector::UpVector * 0.3f, 6, 320.0f, 0.8f, 5.0f, 0.7f, 980.0f);
			SpawnBurst(EFightParticle::Dust, At, Back, 3, 120.0f, 0.6f, 9.0f, 0.7f, 100.0f);
			break;
		case colony::ProjectileHitKind::Armor:
			SpawnBurst(EFightParticle::Spark, At, Back, 12, 560.0f, 0.9f, 3.0f, 0.35f, 980.0f);
			if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
			{
				if (ASimPawnActor* Actor = Pawns->FindPawn(Tracer.Target))
				{
					Actor->PlayHitReaction(Dir, 0.3f);
					++HitReactionsPlayed;
				}
			}
			break;
		case colony::ProjectileHitKind::Pawn:
		{
			SpawnBurst(EFightParticle::Blood, At, Dir, 16, 320.0f, 0.7f, 6.0f, 0.6f, 980.0f);
			const float Strength = FMath::Clamp(Tracer.Amount / 20.0f, 0.35f, 1.0f);
			if (const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>())
			{
				if (ASimPawnActor* Actor = Pawns->FindPawn(Tracer.Target))
				{
					Actor->PlayHitReaction(Dir, Strength);
					++HitReactionsPlayed;
					SpawnBodyDecal(*Actor, At, Dir, Tracer.Amount);
					SpawnGroundDecal(FVector(At.X, At.Y, Actor->GetActorLocation().Z), FMath::Clamp(14.0f + Tracer.Amount * 1.2f, 16.0f, 40.0f));
				}
			}
			if (const USimSubsystem* Bridge = BridgeSource.Get())
			{
				RecentImpulse.Add(entt::to_integral(Tracer.Target), TPair<FVector, int64>(Dir * FMath::Min(Tracer.Amount * FightImpulsePerDamage, FightImpulseMax), Bridge->GetGameSeconds()));
			}
			if (const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>(); Protagonist && Protagonist->IsDirectMode() && Protagonist->GetControlled() == Tracer.Shooter)
			{
				HitMarkerSeconds = 0.18f;
				++HitMarkers;
			}
			break;
		}
		default:
			break;
	}
}

void UFightSubsystem::TrimDecals()
{
	Decals.RemoveAll([](const TWeakObjectPtr<UDecalComponent>& Decal) { return !Decal.IsValid(); });
	while (Decals.Num() > FightMaxDecals)
	{
		if (Decals[0].IsValid())
		{
			Decals[0]->DestroyComponent();
		}
		Decals.RemoveAt(0);
	}
}

void UFightSubsystem::SpawnBodyDecal(ASimPawnActor& Actor, const FVector& At, const FVector& Direction, float Amount)
{
	if (!bDecalMaterialTried)
	{
		bDecalMaterialTried = true;
		DecalBase = LoadObject<UMaterialInterface>(nullptr, FightDecalMaterialPath);
		if (DecalBase)
		{
			WoundMaterial = UMaterialInstanceDynamic::Create(DecalBase, this);
			WoundMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.45f, 0.012f, 0.012f));
			PoolMaterial = UMaterialInstanceDynamic::Create(DecalBase, this);
			PoolMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.32f, 0.006f, 0.006f));
		}
		else
		{
			bDecalMaterialMissing = true;
			UE_LOG(LogFights, Warning, TEXT("%s is missing, so there are no wound decals (UnrealEditor-Cmd.exe Rim3D.uproject -run=FightMaterials)"), FightDecalMaterialPath);
		}
	}
	USkeletalMeshComponent* Mesh = Actor.GetDecalMesh();
	if (!WoundMaterial || !Mesh)
	{
		return;
	}
	const float Half = FMath::Clamp(5.0f + Amount * 0.5f, 6.0f, 12.0f);
	const FName Bone = Mesh->FindClosestBone(At);
	// The box straddles the body surface, its X axis along the bullet.
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAttached(WoundMaterial, FVector(14.0f, Half, Half), Mesh, Bone, At - Direction.GetSafeNormal() * 6.0f,
		Direction.Rotation(), EAttachLocation::KeepWorldPosition, 0.0f);
	if (Decal)
	{
		Decal->SetFadeScreenSize(0.0f);
		Decals.Add(Decal);
		++BodyDecals;
		TrimDecals();
	}
}

void UFightSubsystem::SpawnGroundDecal(const FVector& At, float RadiusCm)
{
	if (!PoolMaterial)
	{
		return;
	}
	// Straight down, a random turn so the blobs don't line up.
	UDecalComponent* Decal = UGameplayStatics::SpawnDecalAtLocation(GetWorld(), PoolMaterial, FVector(40.0f, RadiusCm, RadiusCm), At + FVector(Random.FRandRange(-20.0f, 20.0f), Random.FRandRange(-20.0f, 20.0f), 10.0f),
		FRotator(-90.0f, Random.FRandRange(0.0f, 360.0f), 0.0f), 0.0f);
	if (Decal)
	{
		Decal->SetFadeScreenSize(0.0f);
		Decals.Add(Decal);
		++GroundDecals;
		TrimDecals();
	}
}

int32 UFightSubsystem::GetLiveDecals() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<UDecalComponent>& Decal : Decals)
	{
		Count += Decal.IsValid() ? 1 : 0;
	}
	return Count;
}

// ---------------------------------------------------------------------------
// Per frame.

void UFightSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	USimSubsystem* Bridge = BridgeSource.Get();
	if (!Bridge || !Bridge->GetSim())
	{
		return;
	}
	if (Bridge->GetSimGeneration() != LastGeneration)
	{
		LastGeneration = Bridge->GetSimGeneration();
		ResetEffects();
	}
	UpdatePlayerInput();
	if (EffectsOn())
	{
		EnsureEffects();
		if (Holder && SolidMaterial)
		{
			const float GameDelta = DeltaTime * static_cast<float>(Bridge->GetSpeed());
			UpdateTracers(DeltaTime, GameDelta);
			UpdateParticles(GameDelta);
			UpdateLights(GameDelta);
			SyncPawns();
		}
	}
	HitMarkerSeconds = FMath::Max(0.0f, HitMarkerSeconds - DeltaTime);
	EquipMessageSeconds = FMath::Max(0.0f, EquipMessageSeconds - DeltaTime);
	DrawHitboxes();
	DrawHud();
}

void UFightSubsystem::UpdateTracers(float DeltaTime, float GameDelta)
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	TArray<FTransform> Transforms;
	Transforms.Reserve(Tracers.Num());

	for (int32 Index = Tracers.Num() - 1; Index >= 0; --Index)
	{
		FFightTracer& Tracer = Tracers[Index];
		Tracer.Age += DeltaTime;

		// The bullet's own direction and speed, from the snapshot once it shows there.
		if (!Tracer.bDirKnown)
		{
			const colony::ProjectileView* Found = nullptr;
			for (const colony::ProjectileView& View : Snapshot.projectiles)
			{
				if (View.id == Tracer.Id)
				{
					Found = &View;
					break;
				}
			}
			if (Found)
			{
				const FVector Velocity(Found->vx, Found->vy, Found->vz);
				if (Velocity.SizeSquared() > 1.0e-8)
				{
					Tracer.Dir = Velocity.GetSafeNormal();
					Tracer.Speed = static_cast<float>(Velocity.Size() * 100.0 * colony::kStepsPerRealSecondAt1x);
					Tracer.bDirKnown = true;
				}
			}
			else if (Tracer.bEnded)
			{
				// Fired and stopped inside one step: it never showed in a snapshot.
				Tracer.bDirKnown = true;
			}
			else if (++Tracer.Frames > 6)
			{
				++TracersDropped;
				Tracers.RemoveAtSwap(Index);
				continue;
			}
		}
		if (!Tracer.bDirKnown)
		{
			continue;
		}

		float ChordLength = TNumericLimits<float>::Max();
		if (Tracer.bEnded)
		{
			const FVector Chord = Tracer.End - Tracer.Start;
			ChordLength = static_cast<float>(Chord.Size());
			if (ChordLength > 1.0f)
			{
				Tracer.Dir = Chord / ChordLength;
			}
		}
		Tracer.Distance += Tracer.Speed * GameDelta;

		if (Tracer.bEnded && Tracer.Distance >= ChordLength)
		{
			++TracersArrived;
			const FFightTracer Done = Tracer;
			Tracers.RemoveAtSwap(Index);
			SpawnImpact(Done);
			continue;
		}
		if (Tracer.Age > FightTracerMaxAge || Tracer.Distance > FightTracerMaxDistance)
		{
			++TracersDropped;
			Tracers.RemoveAtSwap(Index);
			continue;
		}

		const float Length = FMath::Clamp(Tracer.Speed * GameDelta * 1.5f, 150.0f, 1800.0f);
		const float TailDistance = FMath::Max(0.0f, Tracer.Distance - Length);
		const float Segment = Tracer.Distance - TailDistance;
		if (Segment > 20.0f)
		{
			Transforms.Add(FTransform(FRotationMatrix::MakeFromZ(Tracer.Dir).ToQuat(), Tracer.Start + Tracer.Dir * TailDistance, FVector(1.0, 1.0, Segment / 100.0f)));
		}
	}

	if (TracerInstances)
	{
		TracerInstances->ClearInstances();
		if (!Transforms.IsEmpty())
		{
			TracerInstances->AddInstances(Transforms, false, true);
		}
	}
}

void UFightSubsystem::UpdateParticles(float GameDelta)
{
	TArray<TArray<FTransform>> ByKind;
	ByKind.SetNum(static_cast<int32>(EFightParticle::Count));
	for (int32 Index = Particles.Num() - 1; Index >= 0; --Index)
	{
		FParticle& Particle = Particles[Index];
		Particle.Age += GameDelta;
		if (Particle.Age >= Particle.Life)
		{
			Particles.RemoveAtSwap(Index);
			continue;
		}
		Particle.Velocity.Z -= Particle.Gravity * GameDelta;
		Particle.Velocity *= FMath::Max(0.0f, 1.0f - Particle.Drag * GameDelta);
		Particle.Position += Particle.Velocity * GameDelta;
		const float T = Particle.Age / Particle.Life;
		float Scale = Particle.SizeCm / 10.0f;
		if (Particle.Kind == EFightParticle::Flash)
		{
			Scale *= 1.0f - T * 0.7f;
		}
		else if (Particle.Kind == EFightParticle::Smoke)
		{
			Scale *= (0.4f + T * 1.4f) * (T > 0.7f ? (1.0f - T) / 0.3f : 1.0f);
		}
		else if (T > 0.7f)
		{
			Scale *= (1.0f - T) / 0.3f;
		}
		ByKind[static_cast<int32>(Particle.Kind)].Add(FTransform(FRotator(T * 400.0f, T * 700.0f, 0.0f), Particle.Position, FVector(Scale)));
	}
	for (int32 Kind = 0; Kind < ByKind.Num(); ++Kind)
	{
		if (UInstancedStaticMeshComponent* Component = ParticleInstances.IsValidIndex(Kind) ? ParticleInstances[Kind].Get() : nullptr)
		{
			if (Component->GetInstanceCount() > 0 || !ByKind[Kind].IsEmpty())
			{
				Component->ClearInstances();
				if (!ByKind[Kind].IsEmpty())
				{
					Component->AddInstances(ByKind[Kind], false, true);
				}
			}
		}
	}
}

void UFightSubsystem::UpdateLights(float GameDelta)
{
	for (int32 Index = 0; Index < Lights.Num(); ++Index)
	{
		if (!Lights[Index] || LightAges[Index] >= FightLightSeconds)
		{
			continue;
		}
		LightAges[Index] += GameDelta;
		if (LightAges[Index] >= FightLightSeconds)
		{
			Lights[Index]->SetVisibility(false);
		}
		else
		{
			Lights[Index]->SetIntensity(FightLightIntensity * FMath::Square(1.0f - LightAges[Index] / FightLightSeconds));
		}
	}
}

UStaticMeshComponent* UFightSubsystem::GetHeldGun(colony::Entity Pawn) const
{
	const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	return Weapons ? Weapons->GetHandsComponent(Pawn) : nullptr;
}

int32 UFightSubsystem::GetHeldGuns() const
{
	const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
	return Weapons ? Weapons->CountWeaponsInHands() : 0;
}

void UFightSubsystem::SpawnMeleeImpact(colony::ProjectileHitKind Kind, colony::Entity Attacker, colony::Entity Target, const FVector& At,
	const FVector& Direction, int32 Part, float Amount)
{
	if (!EffectsOn())
	{
		return;
	}
	EnsureEffects();
	FFightTracer Blow;
	Blow.Id = colony::kNoEntity;
	Blow.Shooter = Attacker;
	Blow.Start = At - Direction.GetSafeNormal() * 60.0f;
	Blow.Dir = Direction;
	Blow.bDirKnown = true;
	Blow.bEnded = true;
	Blow.End = At;
	Blow.HitKind = Kind;
	Blow.Target = Target;
	Blow.Part = Part;
	Blow.Amount = Amount;
	SpawnImpact(Blow);
}
// Downed and dead pawns fall; pawns that get up are put back on their feet; armed pawns show their gun.
void UFightSubsystem::SyncPawns()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const USimPawnSubsystem* Pawns = GetWorld()->GetSubsystem<USimPawnSubsystem>();
	if (!Bridge || !Pawns)
	{
		return;
	}
	const int64 Now = Bridge->GetGameSeconds();
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		if (View.category != colony::ThingCategory::Pawn)
		{
			continue;
		}
		ASimPawnActor* Actor = Pawns->FindPawn(View.id);
		if (!Actor)
		{
			continue;
		}
		const bool bDown = View.downed || View.dead;
		if (bDown && !Actor->IsRagdoll())
		{
			FVector Impulse = FVector::ZeroVector;
			if (const TPair<FVector, int64>* Recent = RecentImpulse.Find(entt::to_integral(View.id)); Recent && Now - Recent->Value <= 3)
			{
				Impulse = Recent->Key;
			}
			Actor->StartRagdoll(Impulse);
			if (Actor->IsRagdoll())
			{
				++RagdollsStarted;
			}
		}
		else if (!bDown && Actor->IsRagdoll())
		{
			Actor->StopRagdoll();
		}
	}
}

// ---------------------------------------------------------------------------
// The protagonist's gun.

bool UFightSubsystem::ComputeAimPoint(FVector& OutWorldPoint, colony::Entity* OutPawnHit) const
{
	const USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (OutPawnHit)
	{
		*OutPawnHit = colony::kNoEntity;
	}
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || !Protagonist->GetBody())
	{
		return false;
	}
	const AProtagonistPawn* Body = Protagonist->GetBody();
	const FVector Direction = Body->GetViewDirection().GetSafeNormal();
	// Third person: the ray starts at the colonist's side of the camera, so a wall behind the colonist doesn't count.
	// (The view's distance now: closer while aiming, none in first person.)
	const FVector Start = Body->GetViewLocation() + Direction * Body->GetEffectiveArmLength();
	const FVector A = (Start - Bridge->GetWorldOrigin()) / 100.0;
	const FVector B = A + Direction * FightAimRangeM;
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	const colony::CellPos OwnCell{FMath::FloorToInt32(View ? View->x : 0.0f), FMath::FloorToInt32(View ? View->y : 0.0f), 0};
	const colony::TraceHit Hit = Bridge->GetSim()->combat().trace(static_cast<float>(A.X), static_cast<float>(A.Y), static_cast<float>(A.Z),
		static_cast<float>(B.X), static_cast<float>(B.Y), static_cast<float>(B.Z), 0, Pawn, OwnCell);
	if (Hit.kind != colony::ProjectileHitKind::None)
	{
		OutWorldPoint = FightSimToWorldUnits(*Bridge, Hit.x, Hit.y, Hit.z);
		if (OutPawnHit && Hit.kind == colony::ProjectileHitKind::Pawn)
		{
			*OutPawnHit = Hit.thing;
		}
	}
	else
	{
		OutWorldPoint = FightSimToWorldUnits(*Bridge, static_cast<float>(B.X), static_cast<float>(B.Y), static_cast<float>(B.Z));
	}
	return true;
}

bool UFightSubsystem::Fire()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead)
	{
		return false;
	}
	if (!View->weapon.valid() || !Bridge->GetDefs() || !Bridge->GetDefs()->get(View->weapon).has("verbs"))
	{
		// No gun in hand: nothing to fire (a melee weapon or fists swing instead, UWeaponDrawSubsystem).
		bNoGunMessage = true;
		return false;
	}
	// One order per sim step; and only when the gun can take it (the sim refuses anything else).
	if (Bridge->GetGameSeconds() <= LastFireStep || Bridge->GetSim()->combat().shotReadyIn(Pawn) > 0)
	{
		return false;
	}
	FVector Aim;
	if (!ComputeAimPoint(Aim))
	{
		return false;
	}
	const FVector Sim = (Aim - Bridge->GetWorldOrigin()) / 100.0;
	Bridge->Submit(colony::ShootCommand{Pawn, colony::kNoEntity, static_cast<float>(Sim.X), static_cast<float>(Sim.Y), static_cast<float>(Sim.Z), true});
	LastFireStep = Bridge->GetGameSeconds();
	++FireOrders;
	return true;
}

bool UFightSubsystem::Reload()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::CombatSystem& Combat = Bridge->GetSim()->combat();
	if (Combat.magazineSize(Pawn) <= 0 || Combat.roundsLeft(Pawn) >= Combat.magazineSize(Pawn) || Combat.reloading(Pawn))
	{
		return false;
	}
	Bridge->Submit(colony::ReloadCommand{Pawn});
	return true;
}

bool UFightSubsystem::ToggleCrouch()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* View = Bridge && Protagonist && Protagonist->IsDirectMode() ? Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	if (!View)
	{
		return false;
	}
	Bridge->Submit(colony::SetStanceCommand{View->id, View->stance == colony::Stance::Crouching ? colony::Stance::Standing : colony::Stance::Crouching});
	return true;
}

bool UFightSubsystem::ToggleProne()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	const colony::ThingView* View = Bridge && Protagonist && Protagonist->IsDirectMode() ? Bridge->GetCurrentSnapshot().find(Protagonist->GetControlled()) : nullptr;
	if (!View)
	{
		return false;
	}
	Bridge->Submit(colony::SetStanceCommand{View->id, View->stance == colony::Stance::Prone ? colony::Stance::Standing : colony::Stance::Prone});
	return true;
}

bool UFightSubsystem::EquipNearest()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Bridge->GetDefs() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return false;
	}
	const colony::Snapshot& Snapshot = Bridge->GetCurrentSnapshot();
	const colony::ThingView* Me = Snapshot.find(Protagonist->GetControlled());
	if (!Me || Me->downed || Me->dead)
	{
		return false;
	}
	colony::Entity Best = colony::kNoEntity;
	double BestDistance = FightEquipRangeM;
	for (const colony::ThingView& View : Snapshot.things)
	{
		if (View.category != colony::ThingCategory::Item || !View.def.valid())
		{
			continue;
		}
		const colony::Def& Def = Bridge->GetDefs()->get(View.def);
		if (Def.text("equipmentType") != "Primary" || !Def.has("verbs"))
		{
			continue;
		}
		const double Distance = FVector2D::Distance(FVector2D(Me->x, Me->y), FVector2D(View.x, View.y));
		if (Distance < BestDistance)
		{
			Best = View.id;
			BestDistance = Distance;
		}
	}
	if (Best == colony::kNoEntity)
	{
		EquipMessageSeconds = 2.0f;
		return false;
	}
	Bridge->Submit(colony::EquipCommand{Me->id, Best});
	return true;
}

void UFightSubsystem::UpdatePlayerInput()
{
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	APlayerController* PC = UGameplayStatics::GetPlayerController(GetWorld(), 0);
	if (!Protagonist || !Protagonist->IsDirectMode() || !PC)
	{
		return;
	}
	// With a building in hand the mouse button plans it and R turns it (UHandsOnSubsystem), so the gun stays quiet.
	const UOrdersSubsystem* Orders = GetWorld()->GetSubsystem<UOrdersSubsystem>();
	const bool bBuilding = Orders && Orders->IsDirectBuilding();
	if (!bBuilding && PC->IsInputKeyDown(EKeys::LeftMouseButton))
	{
		Fire();
	}
	if (!bBuilding && PC->WasInputKeyJustPressed(EKeys::R))
	{
		Reload();
	}
	if (PC->WasInputKeyJustPressed(EKeys::C))
	{
		ToggleCrouch();
	}
	if (PC->WasInputKeyJustPressed(EKeys::Z))
	{
		ToggleProne();
	}
	if (PC->WasInputKeyJustPressed(EKeys::E))
	{
		EquipNearest();
	}
	// Left Shift rolls the way the movement keys point (U21); G throws a grenade at the crosshair.
	if (PC->WasInputKeyJustPressed(EKeys::LeftShift))
	{
		Roll(FVector2D((PC->IsInputKeyDown(EKeys::D) ? 1.0 : 0.0) - (PC->IsInputKeyDown(EKeys::A) ? 1.0 : 0.0),
			(PC->IsInputKeyDown(EKeys::W) ? 1.0 : 0.0) - (PC->IsInputKeyDown(EKeys::S) ? 1.0 : 0.0)));
	}
	if (!bBuilding && PC->WasInputKeyJustPressed(EKeys::G))
	{
		ThrowGrenade();
	}
}

bool UFightSubsystem::Roll(const FVector2D& Input)
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || !Protagonist->GetBody() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead || View->rollPhase != 0 || Bridge->GetGameSeconds() <= LastFireStep)
	{
		return false;
	}
	// The keys are relative to the view; the sim wants a direction on the map (x east, y south as the map has it).
	const double Yaw = FMath::DegreesToRadians(static_cast<double>(Protagonist->GetBody()->GetYaw()));
	const FVector2D Ahead(FMath::Cos(Yaw), FMath::Sin(Yaw));
	const FVector2D Side(-Ahead.Y, Ahead.X);
	FVector2D Direction = Ahead * Input.Y + Side * Input.X;
	if (Direction.IsNearlyZero())
	{
		Direction = -Ahead;  // no key held: a backstep
	}
	Direction.Normalize();
	Bridge->Submit(colony::DodgeRollCommand{Pawn, static_cast<float>(Direction.X), static_cast<float>(Direction.Y)});
	LastFireStep = Bridge->GetGameSeconds();
	return true;
}

bool UFightSubsystem::ThrowGrenade()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode() || Bridge->GetSpeed() == 0)
	{
		return false;
	}
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);
	if (!View || View->downed || View->dead || Bridge->GetGameSeconds() <= LastFireStep || Bridge->GetSim()->explosives().throwReadyIn(Pawn) > 0)
	{
		return false;
	}
	FVector Aim;
	if (!ComputeAimPoint(Aim))
	{
		return false;
	}
	const FVector Sim = (Aim - Bridge->GetWorldOrigin()) / 100.0;
	Bridge->Submit(colony::ThrowCommand{Pawn, std::string(), colony::kNoEntity, static_cast<float>(Sim.X), static_cast<float>(Sim.Y), true});
	LastFireStep = Bridge->GetGameSeconds();
	return true;
}

// ---------------------------------------------------------------------------
// Debug view and HUD.

void UFightSubsystem::SetShowHitboxes(bool bShow)
{
	GFightHitboxes.AsVariable()->Set(bShow ? 1 : 0, ECVF_SetByCode);
}

bool UFightSubsystem::IsShowingHitboxes() const
{
	return GFightHitboxes.GetValueOnGameThread() != 0;
}

void UFightSubsystem::DrawHitboxes()
{
	HitboxesDrawn = 0;
	const USimSubsystem* Bridge = BridgeSource.Get();
	if (!IsShowingHitboxes() || !Bridge || !Bridge->GetSim())
	{
		return;
	}
	UWorld* World = GetWorld();
	const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(World, 0);
	const FVector CameraLocation = Camera ? Camera->GetCameraLocation() : FVector::ZeroVector;
	std::vector<colony::HitboxView> Boxes;
	for (const colony::ThingView& View : Bridge->GetCurrentSnapshot().things)
	{
		FVector Drawn;
		if (View.category != colony::ThingCategory::Pawn || !Bridge->GetBlendedLocation(View.id, Drawn) || FVector::Dist(Drawn, CameraLocation) > 7000.0)
		{
			continue;
		}
		if (!Bridge->GetSim()->combat().hitboxesOf(View.id, Boxes))
		{
			continue;
		}
		// The sim's hitboxes are where the pawn is after the newest step; the body is drawn between two steps.
		const FVector Offset = FVector(Drawn.X, Drawn.Y, 0.0) - FVector(Bridge->SimToWorld(View.x, View.y, View.level).X, Bridge->SimToWorld(View.x, View.y, View.level).Y, 0.0);
		for (const colony::HitboxView& H : Boxes)
		{
			const FColor Color = FLinearColor::MakeFromHSV8(static_cast<uint8>((H.part * 37) % 256), 230, 255).ToFColor(true);
			const FVector A = FightSimToWorldUnits(*Bridge, H.ax, H.ay, H.az) + Offset;
			if (H.box)
			{
				const FQuat Rotation(FVector::UpVector, FMath::Atan2(H.forwardY, H.forwardX));
				DrawDebugBox(World, A, FVector(H.bx, H.by, H.bz) * 100.0, Rotation, Color, false, -1.0f, SDPG_Foreground, 1.0f);
			}
			else
			{
				const FVector B = FightSimToWorldUnits(*Bridge, H.bx, H.by, H.bz) + Offset;
				const FVector Axis = B - A;
				const double Length = Axis.Size();
				const FQuat Rotation = Length > 0.01 ? FQuat::FindBetweenNormals(FVector::UpVector, Axis / Length) : FQuat::Identity;
				DrawDebugCapsule(World, (A + B) * 0.5, Length * 0.5 + H.radius * 100.0, H.radius * 100.0, Rotation, Color, false, -1.0f, SDPG_Foreground, 1.0f);
			}
			++HitboxesDrawn;
		}
	}
}

void UFightSubsystem::DrawHud()
{
	USimSubsystem* Bridge = BridgeSource.Get();
	const UProtagonistSubsystem* Protagonist = GetWorld()->GetSubsystem<UProtagonistSubsystem>();
	if (!Bridge || !Bridge->GetSim() || !Protagonist || !Protagonist->IsDirectMode())
	{
		return;
	}
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
	const float Scale = FMath::Clamp(Viewport->Size.y / 1080.0f, 1.0f, 2.0f);
	ImGui::GetStyle().FontScaleMain = 1.2f * Scale;
	const float Margin = 12.0f * Scale;
	const colony::Sim& Sim = *Bridge->GetSim();
	const colony::Entity Pawn = Protagonist->GetControlled();
	const colony::ThingView* View = Bridge->GetCurrentSnapshot().find(Pawn);

	// The gun, bottom middle.
	const ImGuiWindowFlags PanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	ImGui::SetNextWindowPos(ImVec2(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y - Margin), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.6f);
	if (View && ImGui::Begin("##FightGun", nullptr, PanelFlags))
	{
		const bool bGun = View->weapon.valid() && Bridge->GetDefs() && Bridge->GetDefs()->get(View->weapon).has("verbs");
		const UWeaponDrawSubsystem* Weapons = GetWorld()->GetSubsystem<UWeaponDrawSubsystem>();
		if (bGun)
		{
			const FString Label = UTF8_TO_TCHAR(Bridge->GetDefs()->get(View->weapon).label().c_str());
			if (View->magazineSize > 0)
			{
				FightText(FString::Printf(TEXT("%s   %d / %d%s"), *Label, View->rounds, View->magazineSize, View->reloading ? TEXT("   reloading") : TEXT("")));
			}
			else
			{
				FightText(Label);
			}
			ImGui::TextUnformatted("Left mouse fire   R reload   C crouch   Z lie down");
		}
		else if (View->weapon.valid() && Bridge->GetDefs())
		{
			FightText(UTF8_TO_TCHAR(Bridge->GetDefs()->get(View->weapon).label().c_str()));
			ImGui::TextUnformatted("Left mouse swing   Right mouse block   C crouch   Z lie down");
		}
		else
		{
			ImGui::TextUnformatted("Fists   Left mouse punch   E picks up a gun");
			if (EquipMessageSeconds > 0.0f)
			{
				ImGui::TextUnformatted("No gun within reach");
			}
		}
		if (Weapons)
		{
			const FString Slots = Weapons->DescribeCarried(*View);
			if (!Slots.IsEmpty())
			{
				FightText(Slots);
			}
		}
	}
	ImGui::End();

	// A flash of red lines round the crosshair when a shot of yours hits someone.
	if (HitMarkerSeconds > 0.0f)
	{
		const ImVec2 Centre(Viewport->Pos.x + Viewport->Size.x * 0.5f, Viewport->Pos.y + Viewport->Size.y * 0.5f);
		ImDrawList* Draw = ImGui::GetForegroundDrawList();
		const ImU32 Red = IM_COL32(255, 60, 50, 235);
		const float Near = 12.0f * Scale;
		const float Far = 22.0f * Scale;
		for (const float SX : {-1.0f, 1.0f})
		{
			for (const float SY : {-1.0f, 1.0f})
			{
				Draw->AddLine(ImVec2(Centre.x + SX * Near, Centre.y + SY * Near), ImVec2(Centre.x + SX * Far, Centre.y + SY * Far), Red, 2.5f);
			}
		}
	}
	(void)Sim;
	++HudFramesDrawn;
}
